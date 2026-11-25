/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*
  The worker's side of the statements of §10.1, for the native workers (CPython, V8; MVP review
  V8-11): the state of each statement id that is not CLOSED, whether a reply kind may answer the
  action sent, and the id's next state. What a worker keeps besides (its columns, in its own form)
  is the base class of its Statement.
*/

#ifndef MLE_WORKER_COMMON_STATEMENTS_HPP
#define MLE_WORKER_COMMON_STATEMENTS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "common/mailbox.hpp"
#include "common/wire.hpp"
#include "mle/protocol.h"

namespace mle::common {

/** The states of an id that is not CLOSED (§10.1). */
enum class StatementState : uint8_t { kIdle, kRows, kNext };

/** An SQL statement id that is not CLOSED, with what the worker keeps of it (Extra). */
template <class Extra>
struct Statement : Extra {
  StatementState state = StatementState::kIdle;
  /** Opened by PREPARE: its home is IDLE. Else by EXECUTE: its home is CLOSED. */
  bool prepared = false;
  /** PASS_RESULT_TO_CLIENT, given when the id was opened. */
  bool to_client = false;
  /** A prepared statement's number of parameter markers. */
  uint32_t param_count = 0;
};

/**
  One SQL_RESULT for the id of a request's statements (S: a Statement): check() whether its kind
  may answer the action sent in the id's state, then the step for the kind moves the id to its next
  state (§10.1 rule 3; the actions a worker sends are allowed in the id's state, the others it fails
  without sending).
*/
template <class S>
class StatementStep {
 public:
  using Map = std::unordered_map<uint64_t, S>;

  StatementStep(Map &statements, uint64_t id, wire::SqlAction action, bool sent_to_client)
      : m_map(statements), m_id(id), m_action(action), m_sent_to_client(sent_to_client) {
    if (!statements.empty())
      if (const auto it = statements.find(id); it != statements.end()) m_st = &it->second;
    m_to_client = m_st != nullptr ? m_st->to_client : sent_to_client;
  }
  StatementStep(const StatementStep &) = delete;
  StatementStep &operator=(const StatementStep &) = delete;

  /** The id's statement; nullptr: CLOSED. */
  S *statement() const { return m_st; }

  /** Results of the id go to the client (§10.5). */
  bool to_client() const { return m_to_client; }

  /** A reply kind outside §10.1's table for the action in the id's state is a protocol failure
      (§1 item 4). ROWS never answers for an id whose results go to the client (§10.5); NEXT_RESULT
      in IDLE or CLOSED has nothing to move to: END or ERROR only. */
  void check(uint8_t kind) const {
    using A = wire::SqlAction;
    if (kind > wire::kMaxSqlResultKind) protocol_failure("an unknown SQL_RESULT kind");
    const bool nothing_next =
        m_action == A::kNextResult && (m_st == nullptr || m_st->state == StatementState::kIdle);
    bool legal = false;
    switch (static_cast<wire::SqlResultKind>(kind)) {
      case wire::SqlResultKind::kError:
        legal = m_action != A::kClose;
        break;
      case wire::SqlResultKind::kOk:
        legal = m_action != A::kFetch && !nothing_next;
        break;
      case wire::SqlResultKind::kRows:
        legal = !m_to_client && !nothing_next &&
                (m_action == A::kExecute || m_action == A::kExecutePrepared || m_action == A::kFetch ||
                 m_action == A::kNextResult);
        break;
      case wire::SqlResultKind::kEnd:
        legal = m_action == A::kNextResult;
        break;
    }
    if (!legal) protocol_failure("an SQL_RESULT of a kind that cannot answer the action in the statement's state");
  }

  /** ERROR (a PREPARE's id stays CLOSED), END, a direct statement's last result: the id goes
      home, IDLE for a prepared statement, CLOSED for a direct one. */
  void home() {
    if (m_st == nullptr) return;
    if (m_st->prepared)
      m_st->state = StatementState::kIdle;
    else
      m_map.erase(m_id);
    m_st = nullptr;
  }

  /** OK: CLOSE closes the id, PREPARE makes it IDLE with its parameter count; else NEXT if
      another result follows, home if not. */
  void ok(bool more_results, uint32_t param_count) {
    if (m_action == wire::SqlAction::kClose) {
      if (m_st != nullptr) m_map.erase(m_id);
      m_st = nullptr;
    } else if (m_action == wire::SqlAction::kPrepare) {
      S &s = m_map[m_id];
      s.prepared = true;
      s.to_client = m_sent_to_client;
      s.param_count = param_count;
      m_st = &s;
    } else if (more_results) {
      open().state = StatementState::kNext;
    } else {
      home();
    }
  }

  /** ROWS: ROWS while the set has more, NEXT if another result follows, else home. The statement
      while it is kept (for the set's columns); nullptr for a direct statement that is CLOSED. */
  S *rows(bool more, bool more_results) {
    if (!more && !more_results && (m_st == nullptr || !m_st->prepared)) {
      home();
      return nullptr;
    }
    S &s = open();
    s.state = more ? StatementState::kRows : more_results ? StatementState::kNext : StatementState::kIdle;
    return &s;
  }

 private:
  /** The id has a result to read: a direct statement gets its entry now. */
  S &open() {
    if (m_st == nullptr) {
      m_st = &m_map[m_id];
      m_st->to_client = m_to_client;
    }
    return *m_st;
  }

  Map &m_map;
  const uint64_t m_id;
  const wire::SqlAction m_action;
  const bool m_sent_to_client;
  bool m_to_client = false;
  S *m_st = nullptr;  // nullptr: CLOSED
};

/** Sends CLOSE for id at depth and takes its OK (§10.1 rule 1); reply() waits for the answer (the
    worker's own way of assembling one). The id is then CLOSED. */
template <class Map, class Reply>
void close_statement(Map &statements, uint32_t depth, uint64_t call_seq, uint64_t id, Reply reply) {
  std::array<std::byte, wire::kSqlCloseSize> close{};
  Writer wr(close);
  (void)(wr.put(uint8_t(wire::SqlAction::kClose)) && wr.put(id));  // no text, no params, flags and the rest 0
  Mailbox::send(uint32_t(wire::Op::kSql), 0, depth, call_seq, close);
  const Message m = reply();
  if (m.discarded || m.body.empty() || m.body[0] != std::byte{uint8_t(wire::SqlResultKind::kOk)})
    protocol_failure("a reply to CLOSE that is not OK");
  statements.erase(id);
}

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_STATEMENTS_HPP
