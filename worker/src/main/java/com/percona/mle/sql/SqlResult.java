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
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The results of a statement (Oracle's SqlResult): one or more result sets or OKs, navigated with
 * {@link #nextResult()}. Rows arrive in chunks; {@link #fetchOne()} asks mysqld for the next
 * chunk when one is used up.
 */
public final class SqlResult implements Iterable<Row> {
  /** One result set's columns, as rows see them. */
  static final class Set {
    final List<Column> columns;
    final Map<String, Integer> index;
    final List<String> labels;
    final ValueMapper mapper;
    final SqlOptions options;

    Set(List<MleSql.Column> cols, ValueMapper mapper, SqlOptions options) {
      List<Column> c = new ArrayList<>(cols.size());
      Map<String, Integer> idx = new HashMap<>();
      List<String> labels = new ArrayList<>();
      for (int i = 0; i < cols.size(); i++) {
        Column col = new Column(cols.get(i));
        c.add(col);
        if (idx.putIfAbsent(col.getColumnLabel(), i) == null) labels.add(col.getColumnLabel());
      }
      this.columns = List.copyOf(c);
      this.index = idx;
      this.labels = List.copyOf(labels);
      this.mapper = mapper;
      this.options = options;
      int n = idx.size() <= SCAN_MAX ? idx.size() : 0;
      this.scanKeys = new String[n];
      this.scanIndexes = new int[n];
      int k = 0;
      if (n > 0)
        for (Map.Entry<String, Integer> e : idx.entrySet()) {
          scanKeys[k] = e.getKey();
          scanIndexes[k++] = e.getValue();
        }
    }

    /** Up to this many labels are found by a scan, not the map (exp note 37: row.x in a loop). */
    private static final int SCAN_MAX = 8;
    private final String[] scanKeys;
    private final int[] scanIndexes;

    /** The index of the first column labelled name, or -1. */
    int indexOf(String name) {
      if (name == null) return -1;
      if (scanKeys.length == 0 && !index.isEmpty()) {
        Integer i = index.get(name);
        return i == null ? -1 : i;
      }
      for (int k = 0; k < scanKeys.length; k++) {
        String key = scanKeys[k];
        if (key == name || (key.length() == name.length() && key.equals(name))) return scanIndexes[k];
      }
      return -1;
    }
  }

  private static final Set NO_COLUMNS = new Set(List.of(), null, null);

  private final Session session;
  private final long id;
  /** A statement run from text: closed with its results. Prepared ones stay open. */
  private final boolean owned;
  private final SqlOptions options;
  private Set set = NO_COLUMNS;
  private List<Object[]> rows = List.of();
  private int pos;
  private boolean hasData;
  private boolean moreRows;
  private boolean moreResults;
  private long affected;
  private long insertId;
  private long execUs;
  private List<Warning> warnings = List.of();
  private boolean replaced;
  /** The current result set's row count (-1: not known), and its rows received so far. */
  private long total;
  private long received;

  /**
   * A statement run from text, with or without parameters (not a prepared one): when mysqld
   * buffered its whole result (it then says how many rows: total >= 0; a cursor's result is not
   * buffered and not drained) it sends it in chunks. The rest is fetched when the routine reads
   * it, or drained into the worker before the routine runs another statement (exp note 44 step 3),
   * so mysqld holds at most one such result (12 §11; with parameters too: independent review C14).
   */
  private final boolean direct;

  SqlResult(Session session, long id, boolean owned, SqlOptions options, MleSql.Reply first) {
    this(session, id, owned, false, options, first);
  }

  SqlResult(Session session, long id, boolean owned, boolean direct, SqlOptions options, MleSql.Reply first) {
    this.session = session;
    this.id = id;
    this.owned = owned;
    this.direct = direct;
    this.options = options;
    load(first);
  }

  private void load(MleSql.Reply r) {
    com.percona.mle.worker.SqlBridge.drained(id);  // a former result set's rows are skipped
    hasData = r.hasRows();
    set = hasData ? new Set(r.columns(), session.mapper(), options) : NO_COLUMNS;
    rows = r.rows();
    pos = 0;
    moreRows = r.moreRows();
    moreResults = r.moreResults();
    affected = r.affectedRows();
    insertId = r.insertId();
    execUs = r.executionMicros();
    warnings = warnings(r);
    total = r.totalRows();
    received = rows.size();
    if (direct && moreRows && total >= 0) com.percona.mle.worker.SqlBridge.deferDrain(id, this::drain);
  }

  /** Another statement is about to run: the unread rows into the worker (see direct). */
  private void drain() {
    if (!moreRows || replaced) return;
    List<Object[]> all = new ArrayList<>(Math.max(0, rows.size() - pos));
    while (pos < rows.size()) all.add(take());
    rows = all;
    pos = 0;
    while (moreRows) {
      MleSql.Reply more = session.request(Session.FETCH, id, null, null, 0, 0);
      all.addAll(more.rows());
      fetched(more, more.rows().size());
    }
  }

  /** A FETCH reply of n rows arrived. */
  private void fetched(MleSql.Reply r, int n) {
    received += n;
    moreRows = r.moreRows();
    moreResults = r.moreResults();
    // The last batch's list is the result's complete one, also when it is empty; an earlier
    // batch's is either that so far, or empty for "nothing new".
    if (!moreRows || !r.warnings().isEmpty()) warnings = warnings(r);
    if (!moreRows) com.percona.mle.worker.SqlBridge.drained(id);
  }

  /** The rows of the current result set not read yet; -1 if not known (mysqld's cursor). */
  public long remainingRows() {
    long here = rows.size() - pos;
    if (!moreRows) return here;
    return total >= 0 ? here + total - received : -1;
  }

  private static List<Warning> warnings(MleSql.Reply r) {
    if (r.warnings().isEmpty()) return List.of();
    List<Warning> w = new ArrayList<>(r.warnings().size());
    // mysqld's levels count from 0 (note, warning, error); Oracle's from 1.
    for (MleSql.Warning x : r.warnings()) w.add(new Warning(x.level() + 1, x.code(), x.message()));
    return List.copyOf(w);
  }

  /** A new execution of the prepared statement replaced these results. */
  void replace() {
    if (moreRows) com.percona.mle.worker.SqlBridge.drained(id);
    replaced = true;
    rows = List.of();
    moreRows = false;
    moreResults = false;
  }

  private void live() {
    if (replaced)
      throw new IllegalStateException("the result was replaced by a new execution of its prepared statement");
  }

  /**
   * The next row's values. Results are forward-only, so the slot is cleared (24 P2): a large
   * result read row by row does not stay in memory as a whole. Lists from the decoder are
   * mutable; immutable ones (empty results) are left as they are.
   */
  private Object[] take() {
    Object[] v = rows.get(pos);
    if (rows instanceof ArrayList<Object[]>) rows.set(pos, null);
    pos++;
    return v;
  }

  /** The next row of the current result set, or null at its end (or for an OK). */
  public Row fetchOne() {
    live();
    while (pos >= rows.size()) {
      if (!moreRows) return null;
      MleSql.Reply r = session.request(Session.FETCH, id, null, null, 0, 0);
      rows = r.rows();
      pos = 0;
      fetched(r, rows.size());
    }
    return new Row(set, take());
  }

  /**
   * The remaining rows of the current result set as read from mysqld (values before any language
   * mapping), for column-wise conversions such as numpy arrays (Phase 7).
   */
  public List<Object[]> fetchRemainingRaw() {
    live();
    List<Object[]> out = new ArrayList<>();
    while (true) {
      while (pos < rows.size()) out.add(take());
      if (!moreRows) return out;
      MleSql.Reply r = session.request(Session.FETCH, id, null, null, 0, 0);
      rows = r.rows();
      pos = 0;
      fetched(r, rows.size());
    }
  }

  /**
   * The remaining rows of the current result set into sink (see RowSink): the rows already here
   * as they are, the later chunks while they are decoded from the mailbox (exp note 44 step 3).
   */
  public void fetchRemainingRaw(RowSink sink) {
    live();
    while (pos < rows.size()) sink.row(take());
    rows = List.of();
    pos = 0;
    final int[] n = new int[1];
    RowSink counting = v -> {
      n[0]++;
      sink.row(v);
    };
    while (moreRows) {
      n[0] = 0;
      fetched(session.fetchInto(id, counting), n[0]);
    }
  }

  /** The statement's options (language mappers convert values with them). */
  public SqlOptions getOptions() {
    return options;
  }

  /** The remaining rows of the current result set. */
  public List<Row> fetchAll() {
    List<Row> all = new ArrayList<>(Math.max(0, rows.size() - pos));
    for (Row r = fetchOne(); r != null; r = fetchOne()) all.add(r);
    return all;
  }

  /** The remaining rows of the current result set, fetched as the iteration goes. */
  @Override
  public java.util.Iterator<Row> iterator() {
    return new java.util.Iterator<>() {
      private Row next;

      @Override
      public boolean hasNext() {
        if (next == null) next = fetchOne();
        return next != null;
      }

      @Override
      public Row next() {
        if (!hasNext()) throw new java.util.NoSuchElementException();
        Row r = next;
        next = null;
        return r;
      }
    };
  }

  /** Whether the current result is a result set (rows and columns), not an OK. */
  public boolean hasData() {
    return hasData;
  }

  /**
   * Moves to the next result (the rows left of the current one are skipped); true if it is a
   * result set. False when there are no more results.
   */
  public boolean nextResult() {
    live();
    if (!moreResults && !moreRows) {
      hasData = false;
      rows = List.of();  // the rows left are skipped here too
      pos = 0;
      return false;
    }
    MleSql.Reply r = session.request(Session.NEXT_RESULT, id, null, null, 0, 0);
    if (r.end()) {  // the rows left were skipped, and nothing follows
      com.percona.mle.worker.SqlBridge.drained(id);
      hasData = false;
      rows = List.of();
      pos = 0;
      moreRows = false;
      moreResults = false;
      return false;
    }
    load(r);
    return hasData;
  }

  public List<Column> getColumns() {
    return set.columns;
  }

  public List<String> getColumnNames() {
    List<String> names = new ArrayList<>(set.columns.size());
    for (Column c : set.columns) names.add(c.getColumnLabel());
    return names;
  }

  public int getColumnCount() {
    return set.columns.size();
  }

  public long getAffectedItemsCount() {
    return affected;
  }

  public long getAutoIncrementValue() {
    return insertId;
  }

  public List<Warning> getWarnings() {
    return warnings;
  }

  public int getWarningsCount() {
    return warnings.size();
  }

  /** The statement's execution time in mysqld, e.g. "0.0012 sec". */
  public String getExecutionTime() {
    return String.format(java.util.Locale.ROOT, "%.4f sec", execUs / 1e6);
  }

  public long getExecutionMicros() {
    return execUs;
  }

  /** Ends the results; a statement run from text is closed in mysqld. */
  public void close() {
    if (moreRows) com.percona.mle.worker.SqlBridge.drained(id);
    if (owned && (moreRows || moreResults) && !replaced) session.request(Session.CLOSE, id, null, null, 0, 0);
    rows = List.of();
    moreRows = false;
    moreResults = false;
    hasData = false;
  }
}
