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

/**
 * Vector similarity for routines (exp note 33): dot products, cosine similarity, Euclidean
 * distance and the k most similar rows, with SIMD and, for large work, threads (at most
 * mle.vector_threads). The helper is part of the worker, so it stays compiled across the
 * account's sessions.
 *
 * <p>Vectors are {@code float[]} (VECTOR values in Java routines), rows {@code float[][]} of one
 * dimension. Java routines call it directly; JavaScript and Python routines reach it with {@code
 * Java.type('com.percona.mle.api.Vectors')} / {@code java.type(...)} and name the metric as a
 * string ('cosine', 'dot', 'euclidean'). They pass a Float32Array, a numpy array, a list or an
 * array of numbers for a vector (the {@code Object} overloads; numbers are rounded to float), and
 * {@link #matrix} turns their 2-D arrays into rows for the batch methods. Their vectors are
 * copied when passed: for many rows use {@link #load}, which keeps them in Java.
 *
 * <p>Results: {@link Matches}, best first: highest cosine similarity or dot product, lowest
 * Euclidean distance (ranked by the exact distance, not by |q|² + |r|² - 2q·r, which loses close
 * vectors far from the origin). A zero vector has cosine similarity 0 with everything; a NaN score
 * is never a match. Lengths, dot products and distances are summed in float, as the vectors are
 * (numpy on float32 arrays has the same range): a vector whose squared length underflows float
 * (every component below about 1e-19) counts as a zero vector, and components or differences
 * above about 1.8e19 overflow to Infinity (cosine then NaN, normalize a zero vector; rows at
 * different such distances tie). The JVM worker only. One query and a batch of queries are computed differently and may order near-ties
 * (equal to ~1e-7) differently; the number of threads never changes a result. A killed
 * statement stops the work within milliseconds.
 */
public final class Vectors {
  private Vectors() {}

  /** How similarity is measured. */
  public enum Metric {
    /** Cosine similarity, highest first. */
    COSINE,
    /** Dot product, highest first. */
    DOT,
    /** Euclidean distance, lowest first. */
    EUCLIDEAN;

    /** By name, ignoring case: cosine, dot, euclidean. */
    public static Metric of(String name) {
      if (name != null)
        for (Metric m : values())
          if (m.name().equalsIgnoreCase(name.trim())) return m;
      throw new IllegalArgumentException("unknown metric '" + name + "' (cosine, dot or euclidean)");
    }
  }

  // -------------------------------------------------------------- two vectors

  /** The dot product of a and b (same dimension). */
  public static float dot(float[] a, float[] b) {
    same(a, b);
    return VectorEngine.dot(a, b);
  }

  /** The cosine similarity of a and b; 0 if either is a zero vector. */
  public static float cosine(float[] a, float[] b) {
    same(a, b);
    float[] lanes = new float[VectorEngine.L];
    return VectorEngine.rowDot(a, b, lanes) * VectorEngine.inverseNorm(a, lanes) * VectorEngine.inverseNorm(b, lanes);
  }

  /** The Euclidean distance between a and b. */
  public static float distance(float[] a, float[] b) {
    same(a, b);
    return VectorEngine.distance(a, b);
  }

  /** Scales v to length 1 in place (a zero vector stays zero); returns v. */
  public static float[] normalize(float[] v) {
    if (v == null) throw new IllegalArgumentException("the vector must not be null");
    float inv = VectorEngine.inverseNorm(v, new float[VectorEngine.L]);
    for (int i = 0; i < v.length; i++) v[i] *= inv;
    return v;
  }

  // ---------------------------------------- JavaScript and Python vectors

  /** A JavaScript or Python vector (Float32Array, numpy array, list) as float[]. */
  public static float[] vector(Object v) {
    return GuestVectors.vector(v, "the vector");
  }

  /** JavaScript or Python rows (a 2-D numpy array, a list of vectors) as float[][]. */
  public static float[][] matrix(Object rows) {
    return GuestVectors.matrix(rows, "the rows");
  }

  /** {@link #dot(float[], float[])} for JavaScript and Python vectors. */
  public static float dot(Object a, Object b) {
    return dot(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #cosine(float[], float[])} for JavaScript and Python vectors. */
  public static float cosine(Object a, Object b) {
    return cosine(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #distance(float[], float[])} for JavaScript and Python vectors. */
  public static float distance(Object a, Object b) {
    return distance(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #nearest(float[], float[][], int, Metric)} for a JavaScript or Python query and rows. */
  public static Matches nearest(Object query, Object rows, int k,
      String metric) {
    return nearest(GuestVectors.vector(query, "the query"), GuestVectors.matrix(rows, "the rows"), k, metric);
  }

  /** {@link #related(float[][], int, Metric)} for JavaScript or Python rows. */
  public static Matches[] related(Object rows, int k, String metric) {
    return related(GuestVectors.matrix(rows, "the rows"), k, metric);
  }

  // ------------------------------------------------------------------ search

  /** The k rows most similar to query. */
  public static Matches nearest(float[] query, float[][] rows, int k, Metric metric) {
    VectorEngine.dimension(rows);
    VectorEngine.checkQuery(query, rows, "the query");
    VectorEngine.checkK(k);
    return VectorEngine.nearest(query, rows, null, k, VectorEngine.code(metric), -1, null);
  }

  /** {@link #nearest(float[], float[][], int, Metric)} with the metric by name. */
  public static Matches nearest(float[] query, float[][] rows, int k, String metric) {
    return nearest(query, rows, k, Metric.of(metric));
  }

  /** For each query, the k rows most similar to it (tiled, for many queries at once). */
  public static Matches[] nearest(float[][] queries, float[][] rows, int k, Metric metric) {
    VectorEngine.dimension(rows);
    if (queries == null) throw new IllegalArgumentException("queries must not be null");
    for (int i = 0; i < queries.length; i++) VectorEngine.checkQuery(queries[i], rows, "query " + i);
    VectorEngine.checkK(k);
    return VectorEngine.nearest(queries, rows, null, k, VectorEngine.code(metric), false, null);
  }

  /** {@link #nearest(float[][], float[][], int, Metric)} with the metric by name. */
  public static Matches[] nearest(float[][] queries, float[][] rows, int k, String metric) {
    return nearest(queries, rows, k, Metric.of(metric));
  }

  /** For each row, the k other rows most similar to it ("related items"). */
  public static Matches[] related(float[][] rows, int k, Metric metric) {
    VectorEngine.dimension(rows);
    VectorEngine.checkK(k);
    return VectorEngine.nearest(rows, rows, null, k, VectorEngine.code(metric), true, null);
  }

  /** {@link #related(float[][], int, Metric)} with the metric by name. */
  public static Matches[] related(float[][] rows, int k, String metric) {
    return related(rows, k, Metric.of(metric));
  }

  // ------------------------------------------- arithmetic (exp note 53)

  /** a + b (a new vector). */
  public static float[] add(float[] a, float[] b) {
    same(a, b);
    float[] r = new float[a.length];
    for (int i = 0; i < r.length; i++) r[i] = a[i] + b[i];
    return r;
  }

  /** a - b (a new vector). */
  public static float[] subtract(float[] a, float[] b) {
    same(a, b);
    float[] r = new float[a.length];
    for (int i = 0; i < r.length; i++) r[i] = a[i] - b[i];
    return r;
  }

  /** v * factor (a new vector). */
  public static float[] scale(float[] v, double factor) {
    if (v == null) throw new IllegalArgumentException("the vector must not be null");
    float[] r = new float[v.length];
    for (int i = 0; i < r.length; i++) r[i] = (float) (v[i] * factor);
    return r;
  }

  /** The Euclidean length of v. */
  public static float norm(float[] v) {
    if (v == null) throw new IllegalArgumentException("the vector must not be null");
    return (float) Math.sqrt(VectorEngine.dot(v, v));
  }

  /** The Manhattan (L1) distance between a and b: the sum of |a[i] - b[i]|. */
  public static float manhattan(float[] a, float[] b) {
    same(a, b);
    double s = 0;
    for (int i = 0; i < a.length; i++) s += Math.abs(a[i] - b[i]);
    return (float) s;
  }

  /**
   * The first dims dimensions of v, scaled to length 1 (a new vector): Matryoshka embeddings
   * (OpenAI dimensions, sentence-transformers truncate_dim) keep their meaning in a prefix, but the
   * prefix must be normalized again for cosine and dot.
   */
  public static float[] truncate(float[] v, int dims) {
    if (v == null) throw new IllegalArgumentException("the vector must not be null");
    if (dims < 1 || dims > v.length)
      throw new IllegalArgumentException("dims must be in [1, " + v.length + "], not " + dims);
    return normalize(java.util.Arrays.copyOf(v, dims));
  }

  /** Scales every row to length 1 in place (zero rows stay zero); returns rows. */
  public static float[][] normalizeAll(float[][] rows) {
    VectorEngine.dimension(rows);
    float[] lanes = new float[VectorEngine.L];
    for (float[] r : rows) {
      float inv = VectorEngine.inverseNorm(r, lanes);
      for (int i = 0; i < r.length; i++) r[i] *= inv;
    }
    return rows;
  }

  /** The mean of the rows (a centroid; mean pooling of token embeddings). */
  public static float[] mean(float[][] rows) {
    VectorEngine.dimension(rows);
    return VectorOps.combine(rows, null, null, true);
  }

  /** The weighted mean of the rows (weights: one per row; mean pooling with an attention mask). */
  public static float[] mean(float[][] rows, float[] weights) {
    VectorEngine.dimension(rows);
    return VectorOps.combine(rows, null, weights, true);
  }

  /** The sum of the rows. */
  public static float[] sum(float[][] rows) {
    VectorEngine.dimension(rows);
    return VectorOps.combine(rows, null, null, false);
  }

  // ------------------------------------- matrices, MMR, groups (exp note 53)

  /** Row i of a with row i of b, for every i: one score each (a and b have as many rows). */
  public static float[] pairwise(float[][] a, float[][] b, String metric) {
    int d = VectorEngine.dimension(a);
    if (VectorEngine.dimension(b) != d && a.length > 0 && b.length > 0)
      throw new IllegalArgumentException("the rows have " + d + " and " + b[0].length + " dimensions");
    return VectorOps.pairwise(a, b, VectorEngine.code(Metric.of(metric)));
  }

  /**
   * Every row of a with every row of b: scores[i][j] (at most 25 million; for the best of each row
   * use nearest, for the close pairs pairs).
   */
  public static float[][] similarity(float[][] a, float[][] b, String metric) {
    int d = VectorEngine.dimension(a);
    if (VectorEngine.dimension(b) != d && a.length > 0 && b.length > 0)
      throw new IllegalArgumentException("the rows have " + d + " and " + b[0].length + " dimensions");
    return VectorOps.similarity(a, b, VectorEngine.code(Metric.of(metric)));
  }

  /**
   * Maximal marginal relevance over candidate rows (e.g. the top 20 of a search), cosine: k of
   * them picked one at a time for relevance to query and against redundancy with those already
   * picked (lambda 1: plain relevance; 0: maximal diversity; 0.5 to 0.8 is usual). Scores: the
   * cosine with the query, in pick order.
   */
  public static Matches mmr(float[] query, float[][] rows, int k, double lambda) {
    VectorEngine.dimension(rows);
    VectorEngine.checkQuery(query, rows, "the query");
    return VectorOps.mmr(query, rows, null, k, lambda, null, null, null);
  }

  /** Rows at least threshold similar to query (cosine, dot) or at most threshold away (euclidean), best first. */
  public static Matches within(float[] query, float[][] rows, double threshold, String metric) {
    VectorEngine.dimension(rows);
    VectorEngine.checkQuery(query, rows, "the query");
    int m = VectorEngine.code(Metric.of(metric));
    return VectorOps.within(query, rows, null, m, threshold, null);
  }

  /** The (at most maxPairs best) pairs of rows at least threshold similar / at most threshold apart. */
  public static Pairs pairs(float[][] rows, double threshold, String metric, int maxPairs) {
    VectorEngine.dimension(rows);
    int m = VectorEngine.code(Metric.of(metric));
    return VectorOps.pairs(rows, null, m, threshold, maxPairs, null);
  }

  /** Groups of rows linked by close pairs (connected components), of at least minSize rows. */
  public static Groups groups(float[][] rows, double threshold, String metric, int minSize) {
    VectorEngine.dimension(rows);
    int m = VectorEngine.code(Metric.of(metric));
    return VectorOps.groups(rows, null, m, threshold, minSize, null);
  }

  /** The positions of the rows to keep without near duplicates (the first of each group). */
  public static long[] dedup(float[][] rows, double threshold, String metric) {
    VectorEngine.dimension(rows);
    int m = VectorEngine.code(Metric.of(metric));
    return VectorOps.dedup(rows, null, m, threshold, null);
  }

  /** k-means of the rows: see {@link VectorSet#kmeans(int, String, int, long)}. */
  public static Clusters kmeans(float[][] rows, int k, String metric, int maxIterations, long seed) {
    if (VectorEngine.dimension(rows) == 0 && rows.length == 0) throw new IllegalArgumentException("no rows");
    return VectorOps.kmeans(rows, null, null, k, VectorEngine.code(Metric.of(metric)), maxIterations, seed);
  }

  // -------------------------- the same for JavaScript and Python (exp note 53)

  /** {@link #add(float[], float[])} for JavaScript and Python vectors. */
  public static float[] add(Object a, Object b) {
    return add(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #subtract(float[], float[])} for JavaScript and Python vectors. */
  public static float[] subtract(Object a, Object b) {
    return subtract(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #scale(float[], double)} for a JavaScript or Python vector. */
  public static float[] scale(Object v, double factor) {
    return scale(GuestVectors.vector(v, "the vector"), factor);
  }

  /** {@link #norm(float[])} for a JavaScript or Python vector. */
  public static float norm(Object v) {
    return norm(GuestVectors.vector(v, "the vector"));
  }

  /** {@link #manhattan(float[], float[])} for JavaScript and Python vectors. */
  public static float manhattan(Object a, Object b) {
    return manhattan(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #truncate(float[], int)} for a JavaScript or Python vector. */
  public static float[] truncate(Object v, int dims) {
    return truncate(GuestVectors.vector(v, "the vector"), dims);
  }

  /**
   * {@link #normalize(float[])} for a JavaScript or Python vector: a normalized copy, the argument
   * is not changed. A Java float[] that JavaScript or Python got from this helper
   * (VectorSet.vector, Clusters.centroid) selects {@link #normalize(float[])} instead, which scales
   * it in place (MVP review API-4).
   */
  public static float[] normalize(Object v) {
    return normalize(GuestVectors.vector(v, "the vector").clone());
  }

  /**
   * {@link #normalizeAll(float[][])} for JavaScript or Python rows: normalized copies, the rows
   * passed are not changed (also a JavaScript or Python list of Java float[] rows). A Java
   * float[][] itself selects {@link #normalizeAll(float[][])}, which scales its rows in place (MVP
   * review API-4).
   */
  public static float[][] normalizeAll(Object rows) {
    float[][] m = GuestVectors.matrix(rows, "the rows");
    float[][] copy = new float[m.length][];
    for (int i = 0; i < copy.length; i++) copy[i] = m[i] == null ? null : m[i].clone();
    return normalizeAll(copy);
  }

  /** {@link #mean(float[][])} for JavaScript or Python rows. */
  public static float[] mean(Object rows) {
    return mean(GuestVectors.matrix(rows, "the rows"));
  }

  /** {@link #mean(float[][], float[])} for JavaScript or Python rows and weights. */
  public static float[] mean(Object rows, Object weights) {
    return mean(GuestVectors.matrix(rows, "the rows"), GuestVectors.vector(weights, "the weights"));
  }

  /** {@link #sum(float[][])} for JavaScript or Python rows. */
  public static float[] sum(Object rows) {
    return sum(GuestVectors.matrix(rows, "the rows"));
  }

  /** {@link #pairwise(float[][], float[][], String)} for JavaScript or Python rows. */
  public static float[] pairwise(Object a, Object b, String metric) {
    return pairwise(GuestVectors.matrix(a, "the first rows"), GuestVectors.matrix(b, "the second rows"), metric);
  }

  /** {@link #similarity(float[][], float[][], String)} for JavaScript or Python rows. */
  public static float[][] similarity(Object a, Object b, String metric) {
    return similarity(GuestVectors.matrix(a, "the first rows"), GuestVectors.matrix(b, "the second rows"), metric);
  }

  /** {@link #mmr(float[], float[][], int, double)} for a JavaScript or Python query and rows. */
  public static Matches mmr(Object query, Object rows, int k, double lambda) {
    return mmr(GuestVectors.vector(query, "the query"), GuestVectors.matrix(rows, "the rows"), k, lambda);
  }

  /** {@link #within(float[], float[][], double, String)} for a JavaScript or Python query and rows. */
  public static Matches within(Object query, Object rows, double threshold,
      String metric) {
    return within(GuestVectors.vector(query, "the query"), GuestVectors.matrix(rows, "the rows"), threshold, metric);
  }

  /** {@link #pairs(float[][], double, String, int)} for JavaScript or Python rows. */
  public static Pairs pairs(Object rows, double threshold, String metric, int maxPairs) {
    return pairs(GuestVectors.matrix(rows, "the rows"), threshold, metric, maxPairs);
  }

  /** {@link #groups(float[][], double, String, int)} for JavaScript or Python rows. */
  public static Groups groups(Object rows, double threshold, String metric, int minSize) {
    return groups(GuestVectors.matrix(rows, "the rows"), threshold, metric, minSize);
  }

  /** {@link #dedup(float[][], double, String)} for JavaScript or Python rows. */
  public static long[] dedup(Object rows, double threshold, String metric) {
    return dedup(GuestVectors.matrix(rows, "the rows"), threshold, metric);
  }

  /** {@link #kmeans(float[][], int, String, int, long)} for JavaScript or Python rows. */
  public static Clusters kmeans(Object rows, int k, String metric, int maxIterations, long seed) {
    return kmeans(GuestVectors.matrix(rows, "the rows"), k, metric, maxIterations, seed);
  }

  /** The principal components of rows of vectors ({@link Pca}): k directions of the largest variance. */
  public static Pca pca(float[][] rows, int k) {
    if (rows == null) throw new IllegalArgumentException("the rows must not be null");
    return Pca.ofRows(rows, k);
  }

  public static Pca pca(Object rows, int k) {
    return pca(GuestVectors.matrix(rows, "the rows"), k);
  }

  // ---------------------------------------------------------------- from SQL

  /**
   * Runs a query and keeps its vectors in Java: SELECT id, vector ... (an integer id, then a
   * VECTOR), or SELECT vector ... (ids are then the row positions). Rows with a NULL vector are
   * skipped ({@link VectorSet#skipped}). Runs SQL, so only in stored procedures (stored functions
   * cannot run SQL), on the routine's thread; the rows count towards mle.sql_result_max.
   */
  public static VectorSet load(String sql, Object... params) {
    return VectorSet.load(sql, params);
  }

  /**
   * Runs a query as {@link #load} and keeps its vectors quantized ('int8' or 'binary', {@link
   * QuantizedSet}), each encoded as it arrives: the float vectors are never kept, so the set takes
   * 4 (int8) or 32 (binary) times less memory; scores are approximate.
   */
  public static QuantizedSet loadQuantized(String type, String sql, Object... params) {
    return QuantizedSet.load(type, sql, params);
  }

  // ----------------------------------------------- the cache (exp note 76)

  /**
   * The set kept under name, or the query's vectors loaded (as {@link #load}) and kept under name:
   * the next calls of the session find them without loading again. Only what a routine asks for
   * by name is kept; a kept set is a snapshot (rows written after the load are not in it: {@link
   * #refresh}), and the cache may forget it (the limit, idle expiry, the session's end), so this
   * loads again when it is not kept. A name belongs to the calling routine's schema; the same name
   * with another query or other parameters is an error (one name, one query).
   */
  public static VectorSet cached(String name, String sql, Object... params) {
    return VectorCache.cached(name, sql, params);
  }

  /** {@link #cached} for a quantized set ({@link #loadQuantized}: 'int8' or 'binary', without the floats). */
  public static QuantizedSet cachedQuantized(String name, String type, String sql, Object... params) {
    return VectorCache.cachedQuantized(name, type, sql, params);
  }

  /**
   * Keeps a set made any way (a view, a quantized set, a projection) under name, replacing what the
   * name held; false when it was not kept (the cache off, or the set larger than the limit). The
   * cache may forget it later (the limit, idle expiry, the heap running short, the session's end).
   */
  public static boolean keep(String name, VectorSet set) {
    if (set == null) throw new IllegalArgumentException("the set must not be null");
    return VectorCache.TABLE.keep(VectorCache.key(name), "vectors", VectorCache.sized(set));
  }

  public static boolean keep(String name, QuantizedSet set) {
    if (set == null) throw new IllegalArgumentException("the set must not be null");
    return VectorCache.TABLE.keep(VectorCache.key(name), set.type(), VectorCache.sized(set));
  }

  /** The set kept under name (a VectorSet or a QuantizedSet), or null. */
  public static Object kept(String name) {
    CacheTable.Sized v = VectorCache.TABLE.kept(VectorCache.key(name));
    return v == null ? null : v.set();
  }

  /**
   * Runs the query of the set kept under name again and keeps the new set (its idle time stays):
   * the new set, or null when the name is not kept. If the load fails, the old set stays.
   */
  public static Object refresh(String name) {
    return VectorCache.refresh(name);
  }

  /** Forgets the set kept under name; false if there was none. */
  public static boolean drop(String name) {
    return VectorCache.TABLE.drop(VectorCache.key(name));
  }

  /** Forgets every kept set of this worker (every schema). */
  public static void dropAll() {
    VectorCache.TABLE.dropAll();
  }

  /** The idle time after which the set kept under name is forgotten (0: never); false if it is not kept. */
  public static boolean expireAfter(String name, long idleMs) {
    return VectorCache.TABLE.expireAfter(VectorCache.key(name), idleMs);
  }

  /** The kept sets, least recently used first, then the last set that was not kept or was forgotten, with why. */
  public static CacheEntry[] cacheInfo() {
    return VectorCache.TABLE.info().stream().map(CacheEntry::new).toArray(CacheEntry[]::new);
  }

  /** The cache's limit in bytes (mle.vector_cache_max; 0: off). */
  public static long cacheMax() {
    return VectorCache.TABLE.max();
  }

  /** The bytes kept now. */
  public static long cacheBytes() {
    return VectorCache.TABLE.total();
  }

  /** The threads this worker's helper uses at most (mle.vector_threads, at most the CPUs). */
  public static int threads() {
    return VectorEngine.THREADS;
  }

  private static void same(float[] a, float[] b) {
    if (a == null || b == null) throw new IllegalArgumentException("the vectors must not be null");
    if (a.length != b.length)
      throw new IllegalArgumentException("the vectors have " + a.length + " and " + b.length + " dimensions");
  }
}
