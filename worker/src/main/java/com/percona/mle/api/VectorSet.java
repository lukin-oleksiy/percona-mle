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
import java.math.BigDecimal;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Vectors read with {@link Vectors#load} and their ids (and labels), kept in Java (JavaScript and
 * Python routines search them without copying the rows). Lives as long as the routine keeps it;
 * nothing is kept between calls. Cosine norms are computed once per set.
 *
 * <p>Besides nearest neighbours (exp note 53): {@link #mmr} (diverse top k), {@link #within}
 * (threshold search), {@link #pairs}, {@link #groups} and {@link #dedup} (near duplicates),
 * {@link #mean} and {@link #meanOf} (centroids, profiles), {@link #kmeans}, {@link #classify}
 * (k nearest neighbours' vote) and {@link #truncate} (Matryoshka embeddings).
 */
public final class VectorSet {
  private final float[][] rows;
  private final long[] ids;
  private final String[] labels;
  private final int skipped;
  private float[] inverseNorms;
  private Map<Long, Integer> byId;

  private VectorSet(float[][] rows, long[] ids, String[] labels, int skipped) {
    this.rows = rows;
    this.ids = ids;
    this.labels = labels;
    this.skipped = skipped;
  }

  static VectorSet load(String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    List<float[]> vectors = new ArrayList<>();
    List<Long> idList = new ArrayList<>();
    List<String> labelList = new ArrayList<>();
    int skipped = 0, d = -1, n = 0;
    boolean withIds = false, withLabels = false;
    for (Row row : Mle.session().runSql(sql, params == null ? new Object[0] : params,
        new com.percona.mle.sql.SqlOptions().passResultToClient(false))) {
      int columns = row.getLength();
      if (n == 0 && skipped == 0) {
        if (columns < 1 || columns > 3)
          throw new IllegalArgumentException(
              "the query must return (id, vector, label), (id, vector) or (vector), not " + columns + " columns");
        withIds = columns >= 2;
        withLabels = columns == 3;
      }
      Object v = row.get(withIds ? 1 : 0);
      if (v == null) {  // skipped whatever its id is (NULL too: an outer join without a vector)
        skipped++;
        continue;
      }
      long id = withIds ? id(row.get(0), n + skipped) : n + skipped;
      if (!(v instanceof float[] f))
        throw new IllegalArgumentException("the " + (withIds ? "second" : "first") + " column must be a VECTOR, not "
            + v.getClass().getSimpleName());
      if (d < 0) d = f.length;
      else if (f.length != d)
        throw new IllegalArgumentException("the vector of row " + (n + skipped) + " (id " + id + ") has " + f.length
            + " dimensions, the first has " + d);
      vectors.add(f);
      idList.add(id);
      if (withLabels) {
        Object l = row.get(2);
        labelList.add(l == null ? null : l instanceof byte[] b ? new String(b, java.nio.charset.StandardCharsets.UTF_8)
            : String.valueOf(l));
      }
      n++;
    }
    long[] ids = new long[n];
    for (int i = 0; i < n; i++) ids[i] = idList.get(i);
    return new VectorSet(vectors.toArray(new float[0][]), ids,
        withLabels ? labelList.toArray(new String[0]) : null, skipped);
  }

  /** An id as a long: a whole number in the BIGINT range, never truncated or wrapped. */
  private static long id(Object v, int row) {
    if (v instanceof Long l) return l;
    if (v instanceof Integer || v instanceof Short || v instanceof Byte) return ((Number) v).longValue();
    if (v instanceof BigInteger b && b.bitLength() < 64) return b.longValue();  // BIGINT UNSIGNED up to 2^63 - 1
    if (v instanceof BigDecimal b) {  // DECIMAL: 5.00 is 5
      try {
        return b.longValueExact();
      } catch (ArithmeticException e) {
        // a fraction, or out of range: the error below
      }
    } else if (v instanceof Double || v instanceof Float) {
      double d = ((Number) v).doubleValue();
      if (d == Math.rint(d) && d >= -0x1p63 && d < 0x1p63) return (long) d;
    }
    throw new IllegalArgumentException("the first column must be an integer id (row " + row + " has "
        + (v == null ? "NULL" : v instanceof Number ? v + ": not a whole number in the BIGINT range"
            : v.getClass().getSimpleName()) + ")");
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

  /** The label of the i-th vector (the third column of the load query; null without one). */
  public String label(int i) {
    return labels == null ? null : labels[i];
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
    VectorEngine.checkQuery(query, rows, "the query");
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
  public Matches nearest(Object query, int k, String metric) {
    return nearest(GuestVectors.vector(query, "the query"), k, Vectors.Metric.of(metric));
  }

  /** For each query, the k vectors most similar to it. */
  public Matches[] nearest(float[][] queries, int k, Vectors.Metric metric) {
    if (queries == null) throw new IllegalArgumentException("queries must not be null");
    for (int i = 0; i < queries.length; i++) VectorEngine.checkQuery(queries[i], rows, "query " + i);
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

  // ------------------------------------------------------- exp note 53

  /**
   * Maximal marginal relevance, cosine: the fetchK vectors nearest to query, then k of them picked
   * one at a time for relevance and against redundancy with those already picked (lambda 1: plain
   * relevance; 0: maximal diversity; 0.5 to 0.8 is usual). Scores: the cosine with the query, in
   * pick order.
   */
  public Matches mmr(float[] query, int k, double lambda, int fetchK) {
    VectorEngine.checkQuery(query, rows, "the query");
    VectorEngine.checkK(k);
    if (fetchK < k) throw new IllegalArgumentException("fetchK (" + fetchK + ") must be at least k (" + k + ")");
    Matches c = nearest(query, fetchK, Vectors.Metric.COSINE);
    return VectorOps.mmr(query, rows, terms(VectorEngine.COSINE), k, lambda, c.indexes(), c.scores(), ids);
  }

  /** {@link #mmr(float[], int, double, int)} with fetchK = 4 k. */
  public Matches mmr(float[] query, int k, double lambda) {
    return mmr(query, k, lambda, (int) Math.min(Integer.MAX_VALUE, 4L * k));
  }

  /** {@link #mmr(float[], int, double, int)} for a JavaScript or Python query. */
  public Matches mmr(Object query, int k, double lambda, int fetchK) {
    return mmr(GuestVectors.vector(query, "the query"), k, lambda, fetchK);
  }

  /** {@link #mmr(float[], int, double)} for a JavaScript or Python query. */
  public Matches mmr(Object query, int k, double lambda) {
    return mmr(GuestVectors.vector(query, "the query"), k, lambda);
  }

  /**
   * Every vector at least threshold similar to query (cosine, dot), or at most threshold away
   * (euclidean), best first.
   */
  public Matches within(float[] query, double threshold, String metric) {
    VectorEngine.checkQuery(query, rows, "the query");
    int m = VectorEngine.code(Vectors.Metric.of(metric));
    return VectorOps.within(query, rows, terms(m), m, threshold, ids);
  }

  /** {@link #within(float[], double, String)} for a JavaScript or Python query. */
  public Matches within(Object query, double threshold, String metric) {
    return within(GuestVectors.vector(query, "the query"), threshold, metric);
  }

  /**
   * The pairs of vectors at least threshold similar (at most threshold apart), best first, at most
   * maxPairs of them (the best): near duplicates, paraphrases. All pairs are compared once (SIMD,
   * threads).
   */
  public Pairs pairs(double threshold, String metric, int maxPairs) {
    int m = VectorEngine.code(Vectors.Metric.of(metric));
    return VectorOps.pairs(rows, terms(m), m, threshold, maxPairs, ids);
  }

  /** {@link #pairs(double, String, int)} with at most 100,000 pairs. */
  public Pairs pairs(double threshold, String metric) {
    return pairs(threshold, metric, 100_000);
  }

  /**
   * Groups of vectors linked by pairs at least threshold similar (connected components; single
   * linkage), of at least minSize vectors, largest first.
   */
  public Groups groups(double threshold, String metric, int minSize) {
    int m = VectorEngine.code(Vectors.Metric.of(metric));
    return VectorOps.groups(rows, terms(m), m, threshold, minSize, ids);
  }

  /** {@link #groups(double, String, int)} with groups of at least 2 (near duplicates). */
  public Groups groups(double threshold, String metric) {
    return groups(threshold, metric, 2);
  }

  /**
   * The ids to keep when near duplicates go: one vector of each group (the first loaded) and every
   * vector without a near duplicate, in set order.
   */
  public long[] dedup(double threshold, String metric) {
    int m = VectorEngine.code(Vectors.Metric.of(metric));
    return VectorOps.dedup(rows, terms(m), m, threshold, ids);
  }

  /** The mean of all vectors (a centroid). */
  public float[] mean() {
    return VectorOps.combine(rows, null, null, true);
  }

  /** The mean of the vectors with these ids (a user profile from the items they liked). */
  public float[] meanOf(long[] ids) {
    return VectorOps.combine(rows, positions(ids), null, true);
  }

  /** The weighted mean of the vectors with these ids (weights: one per id, e.g. ratings or recency). */
  public float[] meanOf(long[] ids, float[] weights) {
    return VectorOps.combine(rows, positions(ids), weights, true);
  }

  /** {@link #meanOf(long[])} for JavaScript or Python ids. */
  public float[] meanOf(Object ids) {
    return meanOf(GuestVectors.longs(ids, "the ids"));
  }

  /** {@link #meanOf(long[], float[])} for JavaScript or Python ids and weights. */
  public float[] meanOf(Object ids, Object weights) {
    return meanOf(GuestVectors.longs(ids, "the ids"), GuestVectors.vector(weights, "the weights"));
  }

  private int[] positions(long[] ids) {
    if (ids == null) throw new IllegalArgumentException("the ids must not be null");
    int[] p = new int[ids.length];
    for (int i = 0; i < p.length; i++)
      if ((p[i] = indexOf(ids[i])) < 0) throw new IllegalArgumentException("no vector with id " + ids[i]);
    return p;
  }

  /**
   * k-means into k clusters ('euclidean', or 'cosine' for spherical k-means), at most
   * maxIterations updates, seeded with java.util.Random(seed): the same input gives the same
   * clusters.
   */
  public Clusters kmeans(int k, String metric, int maxIterations, long seed) {
    if (rows.length == 0) throw new IllegalArgumentException("the set is empty");
    int m = VectorEngine.code(Vectors.Metric.of(metric));
    return VectorOps.kmeans(rows, terms(m), ids, k, m, maxIterations, seed);
  }

  /** {@link #kmeans(int, String, int, long)} with at most 100 updates and seed 42. */
  public Clusters kmeans(int k, String metric) {
    return kmeans(k, metric, 100, 42);
  }

  /**
   * The label most common among the k vectors nearest to query (labels: the third column of the
   * load query); a tie goes to the label of the nearer vector. Null labels do not vote; null when
   * none votes.
   */
  public String classify(float[] query, int k, String metric) {
    if (labels == null)
      throw new IllegalArgumentException("the set has no labels: load (id, vector, label) rows");
    Matches m = nearest(query, k, metric);
    Map<String, int[]> votes = new java.util.LinkedHashMap<>();  // label -> {count}, nearest first
    for (int i = 0; i < m.size(); i++) {
      String l = labels[m.index(i)];
      if (l != null) votes.computeIfAbsent(l, x -> new int[1])[0]++;
    }
    String best = null;
    int most = 0;
    for (Map.Entry<String, int[]> e : votes.entrySet())
      if (e.getValue()[0] > most) {
        most = e.getValue()[0];
        best = e.getKey();
      }
    return best;
  }

  /** {@link #classify(float[], int, String)} for a JavaScript or Python query. */
  public String classify(Object query, int k, String metric) {
    return classify(GuestVectors.vector(query, "the query"), k, metric);
  }

  /**
   * A set of the first dims dimensions of each vector, each scaled back to length 1 (Matryoshka
   * embeddings: shorter vectors, faster search); same ids and labels.
   */
  public VectorSet truncate(int dims) {
    float[][] t = new float[rows.length][];
    for (int i = 0; i < t.length; i++) t[i] = Vectors.truncate(rows[i], dims);
    return new VectorSet(t, ids, labels, skipped);
  }

  /** The principal components of the set's vectors ({@link Pca}): k directions of the largest variance. */
  public Pca pca(int k) {
    return Pca.ofRows(rows, k);
  }

  /**
   * The set in the coordinates of a PCA's components (k dimensions each), same ids and labels: a
   * 2-D or 3-D map of the set, or a smaller set to search.
   */
  public VectorSet project(Pca pca) {
    if (pca == null) throw new IllegalArgumentException("the PCA must not be null");
    return new VectorSet(pca.project(rows), ids, labels, skipped);
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
