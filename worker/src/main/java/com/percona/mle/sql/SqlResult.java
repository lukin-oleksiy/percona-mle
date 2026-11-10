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

  SqlResult(Session session, long id, boolean owned, SqlOptions options, MleSql.Reply first) {
    this.session = session;
    this.id = id;
    this.owned = owned;
    this.options = options;
    load(first);
  }

  private void load(MleSql.Reply r) {
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
    replaced = true;
    rows = List.of();
    moreRows = false;
    moreResults = false;
  }

  private void live() {
    if (replaced)
      throw new IllegalStateException("the result was replaced by a new execution of its prepared statement");
  }

  /** The next row of the current result set, or null at its end (or for an OK). */
  public Row fetchOne() {
    live();
    while (pos >= rows.size()) {
      if (!moreRows) return null;
      MleSql.Reply r = session.request(Session.FETCH, id, null, null, 0, 0);
      rows = r.rows();
      pos = 0;
      moreRows = r.moreRows();
      moreResults = r.moreResults();
      if (!r.warnings().isEmpty()) warnings = warnings(r);
    }
    return new Row(set, rows.get(pos++));
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
      return false;
    }
    load(session.request(Session.NEXT_RESULT, id, null, null, 0, 0));
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
    if (owned && (moreRows || moreResults) && !replaced) session.request(Session.CLOSE, id, null, null, 0, 0);
    rows = List.of();
    moreRows = false;
    moreResults = false;
    hasData = false;
  }
}
