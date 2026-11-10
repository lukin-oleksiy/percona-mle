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

package com.percona.mle.api;

import com.percona.mle.worker.SqlBridge;
import java.util.List;

/**
 * The low-level SQL client of MLE routines (Phase 5, 12-phase5-sql-api.md): statements run in
 * mysqld, in the calling session and transaction, with the routine's privileges; only in stored
 * procedures (in functions and triggers the server refuses with 6120). The SQL APIs of the
 * languages (Oracle's session/SqlResult/... for JavaScript, the same model for Python and Java)
 * are built on it.
 *
 * <p>Statements are identified by ids from {@link #newStatementId()}; all of a call's statements
 * are closed when the routine call returns. A statement run from text is closed by mysqld once
 * its results are read; a prepared one lives until {@link #close}. Errors are {@link
 * MleSqlException}s with MySQL's error number and SQLSTATE; uncaught, they reach the client
 * unchanged.
 */
public final class MleSql {
  private MleSql() {}

  /** Column metadata (Oracle's Column object). */
  public record Column(String name, String table, String type, boolean unsigned, long length, int decimals,
      String originalName, String schema, String originalTable, String charset, String collation, int flags) {}

  public record Warning(int level, int code, String message) {}

  /**
   * One reply: a result set (hasRows, with the first rows; moreRows: {@link #fetch} for more) or
   * an OK (affected rows, insert id); moreResults: {@link #nextResult} for the next result set.
   * Values: Long, BigInteger (BIGINT UNSIGNED), Double, BigDecimal, String, byte[], LocalDate,
   * LocalDateTime, Duration, MleJson, float[] (VECTOR) or null.
   */
  public record Reply(long statementId, boolean hasRows, List<Column> columns, List<Object[]> rows,
      boolean moreRows, boolean moreResults, long affectedRows, long insertId, List<Warning> warnings,
      int parameterCount, long executionMicros) {}

  private static final int EXECUTE = 0;
  private static final int PREPARE = 1;
  private static final int EXECUTE_PREPARED = 2;
  private static final int FETCH = 3;
  private static final int NEXT_RESULT = 4;
  private static final int CLOSE = 5;
  private static final int TO_CLIENT = 1;

  public static long newStatementId() {
    return SqlBridge.newStatementId();
  }

  /**
   * Runs statement text: without parameters directly (the whole result is buffered by mysqld,
   * at most mle.sql_result_max bytes), with parameters as a one-shot prepared statement (rows
   * streamed). toClient: the result sets go to the client (Oracle's passResultToClient).
   */
  public static Reply execute(long id, String sql, Object[] params, boolean toClient, int maxRows) {
    return SqlBridge.request(EXECUTE, id, sql, params, toClient ? TO_CLIENT : 0, maxRows);
  }

  /** Prepares statement text under id; the reply carries the parameter count. */
  public static Reply prepare(long id, String sql, boolean toClient) {
    return SqlBridge.request(PREPARE, id, sql, null, toClient ? TO_CLIENT : 0, 0);
  }

  /** Executes a prepared statement; params bind the leading markers, the others keep their values. */
  public static Reply executePrepared(long id, Object[] params, int maxRows) {
    return SqlBridge.request(EXECUTE_PREPARED, id, null, params, 0, maxRows);
  }

  public static Reply fetch(long id, int maxRows) {
    return SqlBridge.request(FETCH, id, null, null, 0, maxRows);
  }

  public static Reply nextResult(long id) {
    return SqlBridge.request(NEXT_RESULT, id, null, null, 0, 0);
  }

  public static void close(long id) {
    SqlBridge.request(CLOSE, id, null, null, 0, 0);
  }
}
