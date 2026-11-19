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
 * The result of k-means ({@link VectorSet#kmeans}, {@link Vectors#kmeans}): k centroids, each
 * row's cluster and its score to its centroid (the Euclidean distance, or the cosine similarity
 * for spherical k-means with 'cosine'). Outliers: the rows farthest from their centroid. New
 * vectors: {@link #assign}. JSON summary with {@code String(c)} / {@code str(c)}.
 */
public final class Clusters {
  private final float[][] centroids;
  private final int[] assignment;
  private final float[] score;
  private final long[] ids;
  private final int metric;
  private final int iterations;
  private final double inertia;
  private int[][] members;

  Clusters(float[][] centroids, int[] assignment, float[] score, long[] ids, int metric, int iterations,
      double inertia) {
    this.centroids = centroids;
    this.assignment = assignment;
    this.score = score;
    this.ids = ids;
    this.metric = metric;
    this.iterations = iterations;
    this.inertia = inertia;
  }

  /** The number of clusters. */
  public int k() {
    return centroids.length;
  }

  /** The c-th centroid (a copy; unit length for 'cosine'). */
  public float[] centroid(int c) {
    return centroids[c].clone();
  }

  /** All centroids (copies). */
  public float[][] centroids() {
    float[][] r = new float[centroids.length][];
    for (int c = 0; c < r.length; c++) r[c] = centroids[c].clone();
    return r;
  }

  /** The cluster of row i. */
  public int cluster(int i) {
    return assignment[i];
  }

  /** Every row's cluster (a copy). */
  public int[] assignments() {
    return assignment.clone();
  }

  /** Row i's score to its centroid: Euclidean distance, or cosine similarity for 'cosine'. */
  public float score(int i) {
    return score[i];
  }

  /** Every row's score to its centroid (a copy). */
  public float[] scores() {
    return score.clone();
  }

  /** How many rows each cluster has. */
  public int[] sizes() {
    int[] s = new int[centroids.length];
    for (int c : assignment) s[c]++;
    return s;
  }

  /** The rows of cluster c (positions, ascending; a copy). */
  public int[] members(int c) {
    if (members == null) {
      int[] s = sizes();
      int[][] m = new int[s.length][];
      for (int x = 0; x < s.length; x++) m[x] = new int[s[x]];
      int[] fill = new int[s.length];
      for (int i = 0; i < assignment.length; i++) m[assignment[i]][fill[assignment[i]]++] = i;
      members = m;
    }
    return members[c].clone();
  }

  /** The ids of cluster c's rows (positions when there are no ids). */
  public long[] ids(int c) {
    int[] m = members(c);
    long[] r = new long[m.length];
    for (int i = 0; i < r.length; i++) r[i] = ids != null ? ids[m[i]] : m[i];
    return r;
  }

  /** The id of row i (its position when there are no ids). */
  public long id(int i) {
    return ids != null ? ids[i] : i;
  }

  /** Lloyd iterations run (stops early when no row changes cluster). */
  public int iterations() {
    return iterations;
  }

  /** The sum over rows of the squared distance to the centroid ('cosine': of 1 - similarity). */
  public double inertia() {
    return inertia;
  }

  /** The cluster nearest to v. */
  public int assign(float[] v) {
    VectorEngine.checkQuery(v, centroids, "the vector");
    Matches m = VectorEngine.nearest(v, centroids, null, 1, metric, -1, null);
    if (m.size() == 0) throw new IllegalArgumentException("the vector has no finite score to any centroid");
    return m.index(0);
  }

  /** {@link #assign(float[])} for a JavaScript or Python vector. */
  public int assign(Object v) {
    return assign(GuestVectors.vector(v, "the vector"));
  }

  /** JSON: {"k": 8, "iterations": 12, "inertia": 1234.5, "sizes": [...]}; an inertia that is not finite is null. */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("{\"k\": ").append(k()).append(", \"iterations\": ").append(iterations)
        .append(", \"inertia\": ");
    if (Double.isFinite(inertia)) b.append(inertia);
    else b.append("null");  // NaN and Infinity are not JSON
    b.append(", \"sizes\": [");
    int[] s = sizes();
    for (int c = 0; c < s.length; c++) b.append(c > 0 ? ", " : "").append(s[c]);
    return b.append("]}").toString();
  }
}
