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
 * array of numbers for a vector (the {@code Value} overloads; numbers are rounded to float), and
 * {@link #matrix} turns their 2-D arrays into rows for the batch methods. Their vectors are
 * copied when passed: for many rows use {@link #load}, which keeps them in Java.
 *
 * <p>Results: {@link Matches}, best first: highest cosine similarity or dot product, lowest
 * Euclidean distance. A zero vector has cosine similarity 0 with everything; a NaN score is never
 * a match. One query and a batch of queries are computed differently and may order near-ties
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
    return VectorEngine.dot(a, b) * VectorEngine.inverseNorm(a) * VectorEngine.inverseNorm(b);
  }

  /** The Euclidean distance between a and b. */
  public static float distance(float[] a, float[] b) {
    same(a, b);
    return VectorEngine.distance(a, b);
  }

  /** Scales v to length 1 in place (a zero vector stays zero); returns v. */
  public static float[] normalize(float[] v) {
    if (v == null) throw new IllegalArgumentException("the vector must not be null");
    float inv = VectorEngine.inverseNorm(v);
    for (int i = 0; i < v.length; i++) v[i] *= inv;
    return v;
  }

  // ---------------------------------------- JavaScript and Python vectors

  /** A JavaScript or Python vector (Float32Array, numpy array, list) as float[]. */
  public static float[] vector(org.graalvm.polyglot.Value v) {
    return GuestVectors.vector(v, "the vector");
  }

  /** JavaScript or Python rows (a 2-D numpy array, a list of vectors) as float[][]. */
  public static float[][] matrix(org.graalvm.polyglot.Value rows) {
    return GuestVectors.matrix(rows, "the rows");
  }

  /** {@link #dot(float[], float[])} for JavaScript and Python vectors. */
  public static float dot(org.graalvm.polyglot.Value a, org.graalvm.polyglot.Value b) {
    return dot(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #cosine(float[], float[])} for JavaScript and Python vectors. */
  public static float cosine(org.graalvm.polyglot.Value a, org.graalvm.polyglot.Value b) {
    return cosine(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #distance(float[], float[])} for JavaScript and Python vectors. */
  public static float distance(org.graalvm.polyglot.Value a, org.graalvm.polyglot.Value b) {
    return distance(GuestVectors.vector(a, "the first vector"), GuestVectors.vector(b, "the second vector"));
  }

  /** {@link #nearest(float[], float[][], int, Metric)} for a JavaScript or Python query and rows. */
  public static Matches nearest(org.graalvm.polyglot.Value query, org.graalvm.polyglot.Value rows, int k,
      String metric) {
    return nearest(GuestVectors.vector(query, "the query"), GuestVectors.matrix(rows, "the rows"), k, metric);
  }

  /** {@link #related(float[][], int, Metric)} for JavaScript or Python rows. */
  public static Matches[] related(org.graalvm.polyglot.Value rows, int k, String metric) {
    return related(GuestVectors.matrix(rows, "the rows"), k, metric);
  }

  // ------------------------------------------------------------------ search

  /** The k rows most similar to query. */
  public static Matches nearest(float[] query, float[][] rows, int k, Metric metric) {
    int d = VectorEngine.dimension(rows);
    VectorEngine.checkQuery(query, d, "the query");
    VectorEngine.checkK(k);
    return VectorEngine.nearest(query, rows, null, k, VectorEngine.code(metric), -1, null);
  }

  /** {@link #nearest(float[], float[][], int, Metric)} with the metric by name. */
  public static Matches nearest(float[] query, float[][] rows, int k, String metric) {
    return nearest(query, rows, k, Metric.of(metric));
  }

  /** For each query, the k rows most similar to it (tiled, for many queries at once). */
  public static Matches[] nearest(float[][] queries, float[][] rows, int k, Metric metric) {
    int d = VectorEngine.dimension(rows);
    if (queries == null) throw new IllegalArgumentException("queries must not be null");
    for (int i = 0; i < queries.length; i++) VectorEngine.checkQuery(queries[i], d, "query " + i);
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
