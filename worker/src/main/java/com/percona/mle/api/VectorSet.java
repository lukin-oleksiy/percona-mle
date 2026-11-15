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

import com.percona.mle.sql.Row;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Vectors read with {@link Vectors#load} and their ids, kept in Java (JavaScript and Python
 * routines search them without copying the rows). Lives as long as the routine keeps it; nothing
 * is kept between calls. Cosine norms are computed once per set.
 */
public final class VectorSet {
  private final float[][] rows;
  private final long[] ids;
  private final int skipped;
  private float[] inverseNorms;
  private Map<Long, Integer> byId;

  private VectorSet(float[][] rows, long[] ids, int skipped) {
    this.rows = rows;
    this.ids = ids;
    this.skipped = skipped;
  }

  static VectorSet load(String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    List<float[]> vectors = new ArrayList<>();
    List<Long> idList = new ArrayList<>();
    int skipped = 0, d = -1, n = 0;
    boolean withIds = false;
    for (Row row : Mle.session().runSql(sql, params == null ? new Object[0] : params)) {
      int columns = row.getLength();
      if (n == 0 && skipped == 0) {
        if (columns < 1 || columns > 2)
          throw new IllegalArgumentException(
              "the query must return (id, vector) or (vector), not " + columns + " columns");
        withIds = columns == 2;
      }
      Object v = row.get(withIds ? 1 : 0);
      long id = withIds ? id(row.get(0), n + skipped) : n + skipped;
      if (v == null) {
        skipped++;
        continue;
      }
      if (!(v instanceof float[] f))
        throw new IllegalArgumentException("the " + (withIds ? "second" : "first") + " column must be a VECTOR, not "
            + v.getClass().getSimpleName());
      if (d < 0) d = f.length;
      else if (f.length != d)
        throw new IllegalArgumentException("the vector of row " + (n + skipped) + " (id " + id + ") has " + f.length
            + " dimensions, the first has " + d);
      vectors.add(f);
      idList.add(id);
      n++;
    }
    long[] ids = new long[n];
    for (int i = 0; i < n; i++) ids[i] = idList.get(i);
    return new VectorSet(vectors.toArray(new float[0][]), ids, skipped);
  }

  private static long id(Object v, int row) {
    if (v instanceof Number num) return num.longValue();
    throw new IllegalArgumentException("the first column must be an integer id (row " + row + " has "
        + (v == null ? "NULL" : v.getClass().getSimpleName()) + ")");
  }

  /** The number of vectors. */
  public int size() {
    return rows.length;
  }

  /** The vectors' dimension (0 for an empty set). */
  public int dimensions() {
    return rows.length == 0 ? 0 : rows[0].length;
  }

  /** Rows skipped because their vector was NULL. */
  public int skipped() {
    return skipped;
  }

  /** The id of the i-th vector. */
  public long id(int i) {
    return ids[i];
  }

  /** The i-th vector (a copy). */
  public float[] vector(int i) {
    return rows[i].clone();
  }

  /** The position of the vector with this id, or -1. */
  public int indexOf(long id) {
    if (byId == null) {
      Map<Long, Integer> m = new HashMap<>(ids.length * 2);
      for (int i = ids.length - 1; i >= 0; i--) m.put(ids[i], i);  // the first of duplicate ids
      byId = m;
    }
    Integer i = byId.get(id);
    return i == null ? -1 : i;
  }

  /** The k vectors most similar to query. */
  public Matches nearest(float[] query, int k, Vectors.Metric metric) {
    VectorEngine.checkQuery(query, dimensions(), "the query");
    VectorEngine.checkK(k);
    int m = VectorEngine.code(metric);
    return VectorEngine.nearest(query, rows, terms(m), k, m, -1, ids);
  }

  /** {@link #nearest(float[], int, Vectors.Metric)} with the metric by name. */
  public Matches nearest(float[] query, int k, String metric) {
    return nearest(query, k, Vectors.Metric.of(metric));
  }

  /**
   * {@link #nearest(float[], int, Vectors.Metric)} for a JavaScript or Python query (Float32Array,
   * numpy array, list).
   */
  public Matches nearest(org.graalvm.polyglot.Value query, int k, String metric) {
    return nearest(GuestVectors.vector(query, "the query"), k, Vectors.Metric.of(metric));
  }

  /** For each query, the k vectors most similar to it. */
  public Matches[] nearest(float[][] queries, int k, Vectors.Metric metric) {
    if (queries == null) throw new IllegalArgumentException("queries must not be null");
    for (int i = 0; i < queries.length; i++) VectorEngine.checkQuery(queries[i], dimensions(), "query " + i);
    VectorEngine.checkK(k);
    int m = VectorEngine.code(metric);
    return VectorEngine.nearest(queries, rows, terms(m), k, m, false, ids);
  }

  /** {@link #nearest(float[][], int, Vectors.Metric)} with the metric by name. */
  public Matches[] nearest(float[][] queries, int k, String metric) {
    return nearest(queries, k, Vectors.Metric.of(metric));
  }

  /** The k vectors most similar to the one with this id, itself left out. */
  public Matches nearestTo(long id, int k, Vectors.Metric metric) {
    int i = indexOf(id);
    if (i < 0) throw new IllegalArgumentException("no vector with id " + id);
    VectorEngine.checkK(k);
    int m = VectorEngine.code(metric);
    return VectorEngine.nearest(rows[i], rows, terms(m), k, m, i, ids);
  }

  /** {@link #nearestTo(long, int, Vectors.Metric)} with the metric by name. */
  public Matches nearestTo(long id, int k, String metric) {
    return nearestTo(id, k, Vectors.Metric.of(metric));
  }

  /** For each vector, the k other vectors most similar to it ("related items"), in set order. */
  public Matches[] related(int k, Vectors.Metric metric) {
    VectorEngine.checkK(k);
    int m = VectorEngine.code(metric);
    return VectorEngine.nearest(rows, rows, terms(m), k, m, true, ids);
  }

  /** {@link #related(int, Vectors.Metric)} with the metric by name. */
  public Matches[] related(int k, String metric) {
    return related(k, Vectors.Metric.of(metric));
  }

  /** The per-row terms of a metric, computed once. */
  private float[] terms(int metric) {
    if (metric == VectorEngine.COSINE) {
      if (inverseNorms == null) inverseNorms = VectorEngine.rowTerms(rows, metric);
      return inverseNorms;
    }
    return null;  // DOT and EUCLIDEAN need none
  }

  @Override
  public String toString() {
    return "VectorSet(" + rows.length + " vectors of " + dimensions() + " dimensions"
        + (skipped > 0 ? ", " + skipped + " NULL skipped" : "") + ")";
  }
}
