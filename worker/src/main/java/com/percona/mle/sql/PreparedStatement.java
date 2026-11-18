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

package com.percona.mle.sql;

import com.percona.mle.api.MleSql;
import com.percona.mle.api.MleSqlException;
import java.util.ArrayList;
import java.util.List;

/**
 * A prepared statement (Oracle's PreparedStatement), from {@link Session#prepare}. Values given
 * to {@link #bind} bind the markers in order at the next {@link #execute()}; the first execution
 * needs all of them, later ones keep the values of markers not bound again.
 */
public final class PreparedStatement {
  private final Session session;
  private final long id;
  private final SqlOptions options;
  private final int parameterCount;
  private final List<Object> pending = new ArrayList<>();
  private boolean executed;
  private boolean deallocated;
  private SqlResult last;

  PreparedStatement(Session session, long id, SqlOptions options, int parameterCount) {
    this.session = session;
    this.id = id;
    this.options = options;
    this.parameterCount = parameterCount;
  }

  private void open() {
    if (deallocated) throw new IllegalStateException("the prepared statement was deallocated");
  }

  public PreparedStatement bind(Object... values) {
    open();
    if (pending.size() + values.length > parameterCount)
      throw new MleSqlException(1210, "HY000", "Incorrect arguments to bind: " + (pending.size() + values.length)
          + " values for " + parameterCount + " parameter markers");
    for (Object v : values) pending.add(v);
    return this;
  }

  /** Executes with the values bound since the last execution; null with passResultToClient. */
  public SqlResult execute() {
    open();
    if (!executed && pending.size() < parameterCount)
      throw new MleSqlException(1210, "HY000", "Incorrect arguments to EXECUTE: " + pending.size() + " of "
          + parameterCount + " parameter markers bound");
    if (last != null) last.replace();
    last = null;
    Object[] params = session.params(pending.toArray());
    pending.clear();
    MleSql.Reply r = session.request(Session.EXECUTE_PREPARED, id, null, params, 0, 0);
    executed = true;
    if (options.passResultToClient()) return null;
    last = new SqlResult(session, id, false, options, r);
    return last;
  }

  /**
   * Executes the statement once per row, each row giving every parameter marker a value (exp note
   * 51): the rows travel to mysqld in as few messages as fit the mailbox, without a round trip per
   * row. For statements that return no rows (INSERT, UPDATE, DELETE, REPLACE, ...): one that
   * returns a result set is refused after its first row. Stops at the first failing row: the error
   * ({@link MleSqlException#getRow()}) says which; the rows before it stay written (the transaction
   * is the caller's; without one, autocommit commits every row). Returns the sum of the affected
   * rows.
   */
  public SqlResult executeMany(List<?> rows) {
    open();
    if (last != null) last.replace();
    last = null;
    pending.clear();
    long affected = 0, execUs = 0, done = 0;
    List<MleSql.Warning> warnings = List.of();
    int budget = com.percona.mle.worker.SqlBridge.batchBytes();
    List<Object> batch = new ArrayList<>();
    long bytes = 0;
    int inBatch = 0;
    for (int i = 0; i <= rows.size(); i++) {
      Object[] row = null;
      long size = 0;
      if (i < rows.size()) {
        row = session.params(session.row(rows.get(i)));
        int n = row == null ? 0 : row.length;
        if (n != parameterCount)
          throw new MleSqlException(1210, "HY000", "Incorrect arguments to executeMany: row " + i + " has " + n
              + " values for " + parameterCount + " parameter markers", i);
        for (Object v : row) size += size(v);
      }
      if (inBatch > 0 && (row == null || bytes + size > budget)) {
        MleSql.Reply r;
        try {
          r = com.percona.mle.worker.SqlBridge.requestMany(id, batch.toArray(), parameterCount);
        } catch (MleSqlException e) {
          throw e.getRow() < 0 ? e
              : new MleSqlException(e.getErrorCode(), e.getSqlState(), e.getMessage(), done + e.getRow());
        }
        affected += r.affectedRows();
        execUs += r.executionMicros();
        warnings = r.warnings();
        done += inBatch;
        batch.clear();
        bytes = 0;
        inBatch = 0;
      }
      if (row != null) {
        for (Object v : row) batch.add(v);
        bytes += size;
        inBatch++;
      }
    }
    executed = executed || done > 0;
    return new SqlResult(session, id, false, options,
        new MleSql.Reply(id, false, List.of(), List.of(), false, false, affected, 0, warnings, 0, execUs, 0));
  }

  /** About the bytes value v takes in a message (to fill one mailbox arena per batch message). */
  private static long size(Object v) {
    return switch (v) {
      case null -> 1;
      case float[] f -> 16 + 4L * f.length;
      case byte[] b -> 8 + b.length;
      case String s -> 8 + 3L * s.length();
      default -> 24;
    };
  }

  /** Closes the statement in mysqld. */
  public void deallocate() {
    if (deallocated) return;
    deallocated = true;
    if (last != null) last.replace();
    session.request(Session.CLOSE, id, null, null, 0, 0);
  }

  public int getParameterCount() {
    return parameterCount;
  }

  public Object getOption(String name) {
    return options.get(name);
  }
}
