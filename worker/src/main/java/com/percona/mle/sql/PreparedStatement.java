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
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * A prepared statement (Oracle's PreparedStatement), from {@link Session#prepare}. Values given
 * to {@link #bind} bind the markers in order at the next {@link #execute()}; the first execution
 * needs all of them, later ones keep the values of markers not bound again.
 */
public final class PreparedStatement {
  private final Session session;
  private final long id;
  private final String sql;
  private final SqlOptions options;
  private final int parameterCount;
  private final List<Object> pending = new ArrayList<>();
  private boolean executed;
  private boolean deallocated;
  private SqlResult last;

  PreparedStatement(Session session, long id, String sql, SqlOptions options, int parameterCount) {
    this.session = session;
    this.id = id;
    this.sql = sql;
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
   * Executes the statement once per row, each row giving every parameter marker a value (exp notes
   * 51, 52): the rows travel to mysqld in as few messages as fit the mailbox, without a round trip
   * per row. For statements that return no rows (INSERT, UPDATE, DELETE, REPLACE, ...): one that
   * returns a result set is refused after its first row. Stops at the first failing row: the error
   * ({@link MleSqlException#getRow()}) says which; the rows before it stay written (the transaction
   * is the caller's; without one, autocommit commits every statement). Returns the sum of the
   * affected rows.
   *
   * <p>An INSERT or REPLACE of one row of values ({@link BatchRewrite}) into an InnoDB table, with
   * at least {@value #BATCH_MIN_ROWS} rows, runs as multi-row statements of up to 1,000 rows
   * (option batchRewrite, true by default): the server then executes a statement per chunk, not per
   * row. NOW() and the like have one value per statement then, and LAST_INSERT_ID() is the first id
   * of the last statement.
   */
  public SqlResult executeMany(List<?> rows) {
    open();
    if (rows.size() >= BATCH_MIN_ROWS && options.batchRewrite()) {
      BatchRewrite.Form f = form();
      if (f != null && session.innodb(f.schema(), f.table(), false)) return executeRewritten(rows, f);
    }
    return executeRows(rows, 0);
  }

  /**
   * Batches smaller than this run row by row: preparing the multi-row statement in every call (a
   * top-level call closes its statements) costs ~0.2 ms per 1,000 markers, which fewer rows do not
   * win back (exp note 52 §12-13).
   */
  static final int BATCH_MIN_ROWS = 256;
  /** Chunks of a rewritten batch smaller than this (its last rows) run row by row. */
  static final int REWRITE_MIN_ROWS = 16;
  private static final int REWRITE_MAX_ROWS = 1000;
  /** The values one message may carry (kMaxValues in protocol.h). */
  private static final int MAX_VALUES = com.percona.mle.worker.SqlBridge.MAX_VALUES;
  /**
   * Errors a row's values cause (NULL in a NOT NULL column, duplicate key, out of range, truncated
   * or incorrect value, too long, no default, division by zero, foreign key, no partition, CHECK,
   * invalid JSON or VECTOR): only
   * after these a failed chunk is replayed row by row to find the row. Any other error (a lock
   * wait, a deadlock, KILL, a full lock table, ...) is reported for the chunk: the replay could not
   * tell more and would run the rows again in a transaction that may be gone (independent review C24).
   */
  private static final Set<Integer> REPLAY = Set.of(1022, 1048, 1062, 1169, 1216, 1264, 1265, 1292, 1364, 1365,
      1366, 1367, 1406, 1411, 1452, 1526, 1586, 1690, 3140, 3141, 3156, 3819, 6136, 6137, 6138);

  private boolean formChecked;
  private BatchRewrite.Form form;
  /** The multi-row forms prepared so far, by rows per statement (a few; the oldest is deallocated). */
  private final Map<Integer, PreparedStatement> chunks = new LinkedHashMap<>(4, 0.75f, true) {
    @Override
    protected boolean removeEldestEntry(Map.Entry<Integer, PreparedStatement> e) {
      if (size() <= 4) return false;
      e.getValue().deallocate();
      return true;
    }
  };

  private BatchRewrite.Form form() {
    if (!formChecked) {
      formChecked = true;
      form = BatchRewrite.parse(sql, parameterCount);
    }
    return form;
  }

  /**
   * The rows in chunks of the multi-row form (exp note 52): at most 1,000 rows, 65,535 parameters
   * and one mailbox arena of parameters per statement. A chunk that fails because of a row's values
   * (InnoDB rolls the whole statement back) is replayed row by row, which stops at the failing row
   * with its index and writes the rows before it. It is not replayed when the failed statement may
   * have left something behind or the error is not a row's (Session.replayable, REPLAY): the error
   * then carries the chunk's first row and says that the chunk ran as one statement.
   */
  private SqlResult executeRewritten(List<?> rows, BatchRewrite.Form f) {
    if (last != null) last.replace();
    last = null;
    pending.clear();
    int maxRows = Math.min(REWRITE_MAX_ROWS, MAX_VALUES / parameterCount);
    // The chunks run on statements of their own: this one keeps the values bound before the batch,
    // which a later partial bind must not inherit (independent review C11). Rows that run on this statement
    // (a short last chunk, a replay) set the flag again and leave their last row bound.
    executed = false;
    long budget = com.percona.mle.worker.SqlBridge.batchBytes();
    long affected = 0, execUs = 0;
    List<MleSql.Warning> warnings = List.of();
    int n = rows.size();
    for (int i = 0; i < n; ) {
      int from = i, k = 0;
      long bytes = 0;
      List<Object> params = new ArrayList<>();
      while (i < n && k < maxRows) {
        Object[] row = session.params(session.row(rows.get(i)));
        int w = row == null ? 0 : row.length;
        if (w != parameterCount)
          throw new MleSqlException(1210, "HY000", "Incorrect arguments to executeMany: row " + i + " has " + w
              + " values for " + parameterCount + " parameter markers", i);
        long size = 0;
        for (Object v : row) size += size(v);
        if (k > 0 && bytes + size > budget) break;
        Collections.addAll(params, row);
        bytes += size;
        k++;
        i++;
      }
      if (k < REWRITE_MIN_ROWS) {  // a short last chunk: row by row
        SqlResult r = executeRows(rows.subList(from, from + k), from);
        affected += r.getAffectedItemsCount();
        execUs += r.getExecutionMicros();
        warnings = List.of();
        continue;
      }
      try {
        MleSql.Reply r = chunk(f, k).executeMapped(params.toArray());
        affected += r.affectedRows();
        execUs += r.executionMicros();
        warnings = r.warnings();
      } catch (MleSqlException e) {
        // Replaying is only right if the failed statement left nothing behind (looked up now: the
        // table may have changed since the batch was found eligible).
        String why = !REPLAY.contains(e.getErrorCode()) ? "the error is not one of a row's values"
            : session.replayable(f.schema(), f.table());
        if (why != null)
          throw new MleSqlException(e.getErrorCode(), e.getSqlState(), e.getMessage() + " (executeMany: rows "
              + from + " to " + (from + k - 1) + " ran as one statement and are not run again row by row, so the "
              + "failing row is not known: " + why + ")", from);
        SqlResult r = executeRows(rows.subList(from, from + k), from);  // throws at the failing row
        affected += r.getAffectedItemsCount();
        execUs += r.getExecutionMicros();
      }
    }
    return new SqlResult(session, id, false, options,
        new MleSql.Reply(id, false, List.of(), List.of(), false, false, affected, 0, warnings, 0, execUs, 0));
  }

  /** The multi-row form with k rows, prepared once. */
  private PreparedStatement chunk(BatchRewrite.Form f, int k) {
    PreparedStatement p = chunks.get(k);
    if (p == null) {
      p = session.prepare(f.statement(k), new SqlOptions().passResultToClient(false).over(options));
      chunks.put(k, p);
    }
    return p;
  }

  /** Executes with values already mapped by the session (all markers); the reply only. */
  private MleSql.Reply executeMapped(Object[] params) {
    open();
    if (last != null) last.replace();
    last = null;
    pending.clear();
    MleSql.Reply r = session.request(Session.EXECUTE_PREPARED, id, null, params, 0, 0);
    executed = true;
    return r;
  }

  /** executeMany row by row on mysqld's side (kExecuteMany); row indexes in errors count from offset. */
  private SqlResult executeRows(List<?> rows, long offset) {
    open();
    if (last != null) last.replace();
    last = null;
    pending.clear();
    long affected = 0, execUs = 0, done = offset;
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
          throw new MleSqlException(1210, "HY000", "Incorrect arguments to executeMany: row " + (offset + i)
              + " has " + n + " values for " + parameterCount + " parameter markers", offset + i);
        for (Object v : row) size += size(v);
      }
      // A message carries at most MAX_VALUES values (the host's limit, independent review C10).
      if (inBatch > 0 && (row == null || bytes + size > budget || batch.size() + parameterCount > MAX_VALUES)) {
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
    executed = executed || done > offset;  // rows ran on this statement: its last row stays bound
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
    for (PreparedStatement p : chunks.values()) p.deallocate();
    chunks.clear();
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
