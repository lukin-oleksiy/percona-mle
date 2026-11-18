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

  SQL from routines (Phase 5, 12-phase5-sql-api.md): the statements one
  routine call runs through the server's mysql_stmt_* services, on the
  session's thread, in the caller's transaction and with the routine's
  security context (S-3, S-11).

  One SqlRunner per routine call (nested calls have their own): it owns the
  call's statements and closes what is left when the call ends, so no
  statement outlives its call or crosses to another principal.

  Results (S-11, exp note 44): the server buffers a statement's whole
  result (at most mle.sql_result_max bytes; prepared statements too, without
  a cursor), and it goes to the worker in chunks of at most
  mle.sql_chunk_rows rows that fit the mailbox arena. A chunk is written
  straight into the arena from the server's rows (no Values, no encoding
  buffer); a single row larger than the arena goes as Values in a message
  split into arena-sized parts. passResultToClient sends the result sets
  to the client instead.

  Errors (S-3): an inner statement's error stays in the caller's
  diagnostics area and goes to the worker. If the routine goes on (it
  caught the error), the area is cleared before the next statement and at
  the end of the call; if the routine fails with that same error, it
  reaches the client unchanged. A routine that throws its own SqlError
  raises it like SIGNAL.
*/

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "mle/codec.h"

namespace mle {
struct DirectReply;
}

namespace mle_component {

class SqlRunner {
 public:
  /** results_held: the bytes of results that the statements of the session's
      calls in progress hold in the server (this call's and its callers':
      one budget, mle.sql_call_results_max). */
  SqlRunner(MYSQL_THD thd, size_t *results_held);
  ~SqlRunner();
  SqlRunner(const SqlRunner &) = delete;
  SqlRunner &operator=(const SqlRunner &) = delete;

  /** Serves one SQL request of the worker. With direct, a chunk of rows
      may be written into the mailbox arena instead of result. */
  void handle(const mle::SqlMsg &request, mle::SqlResultMsg *result,
              mle::DirectReply *direct = nullptr);

  /** The routine returned: a caught statement error must not stay in the
      diagnostics area. */
  void call_succeeded();
  /**
    The routine failed with error e. True if e is the inner statement's
    error that is still in the diagnostics area (the caller then reports
    nothing more: the client gets it unchanged). Otherwise the stale
    error, if any, is cleared and false is returned.
  */
  bool propagates(const mle::Error &e);
  /**
    The routine threw its own SqlError (sql_errno and SQLSTATE set, not an
    inner statement's): raise it like SIGNAL. False if it cannot be raised
    that way (invalid values, or SQL not allowed here, e.g. in a function);
    the caller then reports it as a guest error.
  */
  bool raise(const mle::Error &e);

  /** Forgets a pending statement error (clears the diagnostics area). */
  void clear() { clear_pending_error(); }

  /** Open statements (tests, limits). */
  size_t open_statements() const { return m_statements.size(); }

 private:
  struct Statement;

  /** True (and the error in result) when the open statements other than
      `except` hold mle.sql_call_results_max bytes of results already. */
  bool over_budget(const Statement *except, mle::SqlResultMsg *result);
  /** Statement s was executed: what its results hold now is counted. */
  void count_results(Statement &s);
  void execute_text(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void prepare(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void execute_prepared(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void execute_many(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void fetch(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void next_result(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  /** After execute or next_result: the reply for the current result set;
      closes a drained statement run from text. */
  void reply(uint64_t id, Statement &s, uint32_t max_rows, uint64_t exec_us,
             mle::SqlResultMsg *result);
  /** The next chunk of rows of the current result set (result: kind,
      columns and exec_us set): into the arena when it can, else as Values
      in result; closes (destroys) a drained statement run from text.
      first: the result set's first chunk: a buffered result's warnings are
      final, they go with it (and with the last), so that the worker has
      them before it reads all rows. */
  void rows(uint64_t id, Statement &s, uint32_t max_rows, bool first,
            mle::SqlResultMsg *result);
  enum class Direct { kWritten, kError, kNotHere };
  /** The chunk straight into m_direct's arena. kNotHere (nothing taken
      from the statement but a row kept for the next chunk): the head or
      one row does not fit; kError: the server failed (read its error). */
  Direct write_rows(uint64_t id, Statement &s, uint32_t max_rows, bool first,
                    mle::SqlResultMsg *result);
  /** Rows of the current result set into result (at most max_rows). */
  bool read_rows(Statement &s, uint32_t max_rows, mle::SqlResultMsg *result);
  /** The result set is drained: its warnings, whether another follows;
      closes a statement run from text that has no more. */
  void finish_result_set(uint64_t id, Statement &s, mle::SqlResultMsg *result);
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
  size_t *m_results_held;
  /** The arena for the request being served (handle), or nullptr. */
  mle::DirectReply *m_direct = nullptr;
  std::map<uint64_t, std::unique_ptr<Statement>> m_statements;
  bool m_context_checked = false;
  bool m_refused = false;
  /** The inner statement error now in the diagnostics area, if any. */
  bool m_pending = false;
  uint32_t m_pending_errno = 0;
  std::string m_pending_sqlstate;
};

/** The error of a request the runner refuses (bad id, limits). */
void sql_error(mle::SqlResultMsg *result, uint32_t sql_errno,
               const std::string &message);

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_SQL_H
