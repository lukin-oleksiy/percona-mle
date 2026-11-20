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

#ifndef COMPONENTS_MLE_COMPONENT_SQL_H
#define COMPONENTS_MLE_COMPONENT_SQL_H

/**
  @file component/sql.h

  SQL from routines (docs/protocol.md §10): the statements one request (a
  CALL) runs through the server's mysql_stmt_* services, on the session's
  thread, in the caller's transaction and with the security context the
  routine runs in.

  One SqlRunner per request: a nested request has its own, so it does not
  see its caller's statement ids, and what a request leaves open is closed
  when its RESULT arrives.

  Statement ids (§10.1). An id is CLOSED (not in the runner), IDLE, ROWS or
  NEXT. A statement opened by PREPARE is at home in IDLE, one opened by
  EXECUTE in CLOSED. CLOSE always closes. An action that is not allowed in
  the id's state gets an ERROR and changes nothing. An allowed action first
  discards what it replaces; an ERROR to it, whatever the reason, sends the
  id home.

  Results (§10.3). The server buffers a statement's whole result (at most
  mle.sql_result_max bytes), and it goes to the worker in batches of at most
  max_rows (or mle.sql_chunk_rows) rows. A batch is written straight into
  the mailbox arena from the server's rows, so a batch of several rows is
  always one chunk, its columns and warnings included; a row that does not
  fit the arena goes alone, as Values in a message of several chunks. A
  result set's columns go with its first batch only. With
  PASS_RESULT_TO_CLIENT the server sends the results to the consumer of the
  statement that invoked the routine, and the reply is one OK (§10.5).

  Errors (§10.8). A statement's error is in the caller's diagnostics area
  when the reply goes to the worker. If the routine goes on, the area is
  cleared before the next statement and at the end of the call. If the
  routine ends with an error it hands back by id, that error reaches the
  client: unchanged if it is still the one in the diagnostics area, else
  raised again with its number and SQLSTATE.

  The session's bounds (§10.7) are counted in one SqlBudget over all
  requests in progress, at every depth and in every worker.
*/

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "mle/codec.h"
#include "mle/worker.h"

namespace mle_component {

/** What the session's routines make mysqld hold through their SQL. */
struct SqlBudget {
  /** Open statements (mle.sql_statements_max). */
  size_t statements = 0;
  /** Bytes the open statements' results hold in the server
      (mle.sql_call_results_max). */
  size_t results_held = 0;
  /** Bytes of the SQL messages whose statements are in progress. */
  uint64_t request_bytes = 0;
};

/** The bound on SqlBudget::request_bytes. */
constexpr uint64_t kSqlRequestBytesMax = 256ULL << 20;

class SqlRunner {
 public:
  SqlRunner(MYSQL_THD thd, SqlBudget *budget);
  ~SqlRunner();
  SqlRunner(const SqlRunner &) = delete;
  SqlRunner &operator=(const SqlRunner &) = delete;

  /** Serves one SQL message of the worker (CallHooks::sql_into): a batch of
      rows may be written into the mailbox arena instead of result. */
  void handle(const mle::SqlMsg &request, mle::SqlResultMsg *result,
              mle::DirectReply *direct);

  /** The Worker answers an SQL message itself, or replaces the reply of the
      one handle() just served (CallHooks::sql_refused): the id's state by
      §10.1, and the ERROR's number, SQLSTATE and message. */
  void refused(mle::SqlRefusal why, mle::wire::SqlAction action,
               uint64_t stmt_id, mle::SqlResultMsg *error);

  /** The routine returned: a statement error it caught must not stay in the
      diagnostics area. */
  void call_succeeded();
  /**
    The routine ended with an error it hands back (§10.8 rule 1): held is
    that ERROR as the host sent it. True if it is the error in the
    diagnostics area now: the caller reports nothing more and the client
    gets it unchanged. Otherwise the area is cleared and false is returned.
  */
  bool propagates(const mle::HeldError &held);

  /** Forgets a pending statement error (clears the diagnostics area). */
  void clear() { clear_pending_error(); }

  /** Open statements of this request. */
  size_t open_statements() const { return m_statements.size(); }

 private:
  struct Statement;
  struct Batch;

  /** The statement of an id, or nullptr (CLOSED). */
  Statement *find(uint64_t id);
  /** Sends the id home: a direct statement is closed, a prepared one is
      IDLE with its results discarded. */
  void send_home(uint64_t id);
  /** True (and the error in result) when the session's open statements
      other than `except` hold mle.sql_call_results_max bytes of results
      already. */
  bool over_budget(const Statement *except, mle::SqlResultMsg *result);
  /** True (and the error in result) at mle.sql_statements_max. */
  bool too_many_statements(mle::SqlResultMsg *result);
  /** Statement s was executed: what its results hold now is counted. */
  void count_results(Statement &s);
  void execute_text(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void prepare(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void execute_prepared(const mle::SqlMsg &request, Statement &s,
                        mle::SqlResultMsg *result);
  void execute_many(const mle::SqlMsg &request, Statement &s,
                    mle::SqlResultMsg *result);
  void next_result(const mle::SqlMsg &request, Statement &s,
                   mle::SqlResultMsg *result);
  /** After an execution or a move to the next result: the reply for the
      result that is current now, and the id's next state. */
  void reply(uint64_t id, Statement &s, uint32_t max_rows, uint64_t exec_us,
             mle::SqlResultMsg *result);
  /** The next batch of the current result set and the id's next state. */
  void batch(uint64_t id, Statement &s, uint32_t max_rows, uint64_t exec_us,
             mle::SqlResultMsg *result);
  enum class Direct { kWritten, kError, kNotHere };
  /** The batch straight into the arena. kNotHere (nothing taken from the
      statement but a row kept for later): the head or the first row does
      not fit; kError: a row could not be read. */
  Direct write_rows(uint64_t id, Statement &s, Batch &b);
  /** The current result set is complete: whether another result follows,
      and the id's next state (NEXT or home). */
  bool finish_result_set(uint64_t id, Statement &s);
  void statement_error(Statement &s, mle::SqlResultMsg *result);
  /** A new statement the server refused: its error (statement_error), the
      statement closed, the refusal of SQL in a function explained. */
  void statement_failed(std::unique_ptr<Statement> s,
                        mle::SqlResultMsg *result);
  /** The refusal of SQL in a stored function or trigger, in the routine
      author's terms (and out of the diagnostics area). */
  void explain_refusal(mle::SqlResultMsg *result);
  void clear_pending_error();
  void close(uint64_t id);
  /** True (and the server's error in result) if statements are not allowed
      here: in a stored function or trigger. */
  bool refused_here(mle::SqlResultMsg *result);

  MYSQL_THD m_thd;
  SqlBudget *m_budget;
  /** The arena for the message being served (handle), or nullptr. */
  mle::DirectReply *m_direct = nullptr;
  std::map<uint64_t, std::unique_ptr<Statement>> m_statements;
  bool m_context_checked = false;
  bool m_refused = false;
  /** handle() just ran an allowed action of this id and its reply is not an
      ERROR: if the Worker replaces the reply by one, the id goes home. */
  bool m_ran = false;
  uint64_t m_ran_id = 0;
  /** The statement error now in the diagnostics area, if any. */
  bool m_pending = false;
  uint32_t m_pending_errno = 0;
  std::string m_pending_sqlstate;
  std::string m_pending_message;
};

/** An ERROR the host makes itself (a wrong state, a limit): SQLSTATE HY000. */
void sql_error(mle::SqlResultMsg *result, uint32_t sql_errno,
               const std::string &message);

/** The number, SQLSTATE and message of the ERROR for a refusal. no_sql: the
    message while SQL cannot run at all (serving PREPARE). */
void refusal_error(mle::SqlRefusal why, const char *no_sql,
                   mle::SqlResultMsg *error);

/**
  Raises an SQL error with this number, SQLSTATE and message as the error of
  the statement in progress, as SIGNAL does; the message is cut to the 128
  characters the server takes. False if it cannot be raised: the values are
  not a SIGNAL's (a number outside 1..65535, a SQLSTATE that is not 5
  characters of [0-9A-Z] or is of class 00 or 01), or the server runs no
  statement here (a stored function or trigger). Nothing is left in the
  diagnostics area then.
*/
bool signal_error(MYSQL_THD thd, uint32_t sql_errno, const std::string &sqlstate,
                  const std::string &message);

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_SQL_H
