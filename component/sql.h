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
  @file components/mle/component/sql.h

  SQL from routines (Phase 5, 12-phase5-sql-api.md): the statements one
  routine call runs through the server's mysql_stmt_* services, on the
  session's thread, in the caller's transaction and with the routine's
  security context (S-3, S-11).

  One SqlRunner per routine call (nested calls have their own): it owns the
  call's statements and closes what is left when the call ends, so no
  statement outlives its call or crosses to another principal.

  Results (S-11): a statement run directly buffers its whole result in the
  server (at most mle.sql_result_max bytes) and is sent to the worker at
  once; a prepared statement uses a cursor (one row per fetch, at most
  mle.sql_row_max bytes) and is streamed in chunks of mle.sql_chunk_rows.
  passResultToClient sends the result sets to the client instead.

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

namespace mle_component {

class SqlRunner {
 public:
  explicit SqlRunner(MYSQL_THD thd);
  ~SqlRunner();
  SqlRunner(const SqlRunner &) = delete;
  SqlRunner &operator=(const SqlRunner &) = delete;

  /** Serves one SQL request of the worker. */
  void handle(const mle::SqlMsg &request, mle::SqlResultMsg *result);

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

  void execute_text(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void prepare(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void execute_prepared(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void fetch(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  void next_result(const mle::SqlMsg &request, mle::SqlResultMsg *result);
  /** After execute or next_result: the reply for the current result set;
      closes a drained statement run from text. */
  void reply(uint64_t id, Statement &s, uint32_t max_rows, uint64_t exec_us,
             mle::SqlResultMsg *result);
  /** Rows of the current result set into result (at most max_rows). */
  bool read_rows(Statement &s, uint32_t max_rows, mle::SqlResultMsg *result);
  void statement_error(Statement &s, mle::SqlResultMsg *result);
  void clear_pending_error();
  void close(uint64_t id);

  MYSQL_THD m_thd;
  std::map<uint64_t, std::unique_ptr<Statement>> m_statements;
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
