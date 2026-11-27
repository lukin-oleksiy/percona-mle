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
 * The best rows for one query ({@link Vectors}, {@link VectorSet}), best first: the rows'
 * positions, their scores and, for a {@link VectorSet}, their ids. Scores: the cosine similarity
 * or the dot product (highest first), or the Euclidean distance (lowest first, {@link #distance()}).
 * Fewer than k when there are fewer rows. From JavaScript and Python: {@code m.size()}, {@code
 * m.id(i)}, {@code m.score(i)}, or {@code String(m)} / {@code str(m)} for JSON.
 *
 * <p>Ranked lists from anywhere ({@link #of}: a SQL result, a full-text search) combine with
 * {@link #fuse} (reciprocal rank fusion) and {@link #blend} (weighted, normalized scores): hybrid
 * search (exp note 53).
 */
public final class Matches {
  private final int[] index;
  private final float[] score;
  private final long[] ids;
  private final boolean distance;

  Matches(int[] index, float[] score, long[] ids) {
    this(index, score, ids, false);
  }

  Matches(int[] index, float[] score, long[] ids, boolean distance) {
    this.index = index;
    this.score = score;
    this.ids = ids;
    this.distance = distance;
  }

  /** Whether the scores are distances (lower is better), not similarities. */
  public boolean distance() {
    return distance;
  }

  // ------------------------------------------------------------ ranked lists

  /**
   * A ranked list, best first, from ids and their scores (similarities: higher is better); index(i)
   * is i. For a SQL result: {@code Matches.of(ids, scores)}.
   */
  public static Matches of(long[] ids, float[] scores) {
    return of(ids, scores, false);
  }

  /** {@link #of(long[], float[])}; distance: the scores are distances (lower is better). */
  public static Matches of(long[] ids, float[] scores, boolean distance) {
    if (ids == null || scores == null) throw new IllegalArgumentException("ids and scores must not be null");
    if (ids.length != scores.length)
      throw new IllegalArgumentException(ids.length + " ids but " + scores.length + " scores");
    int[] index = new int[ids.length];
    for (int i = 0; i < index.length; i++) index[i] = i;
    return new Matches(index, scores.clone(), ids.clone(), distance);
  }

  /** {@link #of(long[], float[])} for JavaScript or Python arrays (numpy, lists, typed arrays). */
  public static Matches of(Object ids, Object scores) {
    return of(GuestVectors.longs(ids, "the ids"), GuestVectors.vector(scores, "the scores"), false);
  }

  /** {@link #of(long[], float[], boolean)} for JavaScript or Python arrays. */
  public static Matches of(Object ids, Object scores, boolean distance) {
    return of(GuestVectors.longs(ids, "the ids"), GuestVectors.vector(scores, "the scores"), distance);
  }

  /** Reciprocal rank fusion with k = 60: {@link #fuse(int, Matches...)}. */
  public static Matches fuse(Matches... lists) {
    return fuse(60, lists);
  }

  /**
   * Reciprocal rank fusion (Cormack et al. 2009): each id scores the sum of 1 / (k + rank) over
   * the lists it is in (rank from 1; an id twice in a list counts once there, at its first rank),
   * by id; only ranks count, so lists with scores on different
   * scales (cosine, full-text relevance, distance) combine. Best first; ties: the better best rank,
   * then the lower id. A small k favours the top ranks, a large one spreads the weight.
   */
  public static Matches fuse(int k, Matches... lists) {
    return fuse(k, lists, null);
  }

  /**
   * Weighted reciprocal rank fusion: each list's 1 / (k + rank) times its weight (Elasticsearch's
   * and Qdrant's weighted RRF), to trust one retriever more than another. weights: one per list,
   * finite and not negative.
   */
  public static Matches fuse(int k, Matches[] lists, double[] weights) {
    if (k < 0) throw new IllegalArgumentException("k must not be negative, not " + k);
    java.util.Map<Long, double[]> by = merge(lists);  // id -> {score, best rank}
    checkWeights(lists, weights);
    for (int l = 0; l < lists.length; l++) {
      Matches m = lists[l];
      double w = weights == null ? 1 : weights[l];
      java.util.Set<Long> seen = new java.util.HashSet<>();
      for (int i = 0; i < m.size(); i++) {
        if (!seen.add(m.id(i))) continue;  // once per list (MVP review API-5)
        double[] e = by.get(m.id(i));
        e[0] += w / ((double) k + i + 1);  // in double: k + i + 1 overflows int for a huge k
      }
    }
    return ranked(by);
  }

  /** {@link #fuse(int, Matches[], double[])} for JavaScript or Python arrays. */
  public static Matches fuse(int k, Object lists, Object weights) {
    return fuse(k, GuestVectors.matchesArray(lists), doubles(weights));
  }

  private static double[] doubles(Object weights) {
    float[] w = GuestVectors.vector(weights, "the weights");
    double[] d = new double[w.length];
    for (int i = 0; i < w.length; i++) d[i] = w[i];
    return d;
  }

  private static void checkWeights(Matches[] lists, double[] weights) {
    if (weights == null) return;
    if (lists.length != weights.length)
      throw new IllegalArgumentException(lists.length + " lists but " + weights.length + " weights");
    for (int l = 0; l < weights.length; l++)
      if (!(weights[l] >= 0) || Double.isInfinite(weights[l]))  // NaN too: its ids would rank first
        throw new IllegalArgumentException("weight " + l + " must be a finite number, at least 0, not " + weights[l]);
  }

  /** {@link #fuse(int, Matches...)} for a JavaScript or Python array of Matches. */
  public static Matches fuse(int k, Object lists) {
    return fuse(k, GuestVectors.matchesArray(lists));
  }

  /**
   * Weighted sum of normalized scores, by id: each list's scores are scaled to [0, 1] (min-max;
   * distances flipped, so 1 is the best of the list; all equal: 1), missing ids count 0; an id twice
   * in a list counts once there, at its first place. Best first; ties as in {@link #fuse}. weights: one per list, finite and not negative.
   */
  public static Matches blend(Matches[] lists, double[] weights) {
    return blend(lists, weights, "minmax");
  }

  /**
   * {@link #blend(Matches[], double[])} with another normalizer (hybrid search: a full-text score
   * and a vector score on different scales): 'minmax' (to [0, 1]; Elasticsearch's and
   * OpenSearch's min_max), 'zscore' ((s - mean) / std of the list; OpenSearch), 'l2' (s / sqrt of
   * the sum of squares; Elasticsearch's l2_norm), 'dbsf' (distribution-based: mean ± 3 std mapped
   * to [0, 1], clipped; Qdrant). Distances are turned into similarities first (negated; for 'l2'
   * and 'minmax', max - s). A missing id counts 0.
   */
  public static Matches blend(Matches[] lists, double[] weights, String normalizer) {
    if (lists == null || weights == null) throw new IllegalArgumentException("lists and weights must not be null");
    String how = normalizer == null ? "" : normalizer.trim().toLowerCase(java.util.Locale.ROOT);
    if (!how.equals("minmax") && !how.equals("zscore") && !how.equals("l2") && !how.equals("dbsf"))
      throw new IllegalArgumentException("unknown normalizer '" + normalizer + "' (minmax, zscore, l2, dbsf)");
    checkWeights(lists, weights);
    java.util.Map<Long, double[]> by = merge(lists);
    for (int l = 0; l < lists.length; l++) {
      Matches m = lists[l];
      java.util.Set<Long> seen = new java.util.HashSet<>();
      int[] at = new int[m.size()];
      double[] v = new double[m.size()];
      int c = 0;
      double lo = Double.POSITIVE_INFINITY, hi = Double.NEGATIVE_INFINITY;
      for (int i = 0; i < m.size(); i++) {
        float s = m.score[i];
        if (!Float.isFinite(s) || !seen.add(m.id(i))) continue;  // once per list (MVP review API-5)
        at[c] = i;
        v[c++] = s;
        lo = Math.min(lo, s);
        hi = Math.max(hi, s);
      }
      if (c == 0) continue;
      for (int i = 0; i < c; i++) v[i] = m.distance ? (how.equals("minmax") || how.equals("l2") ? hi - v[i] : -v[i]) : v[i];
      double[] x = normalize(v, c, how, m.distance ? 0 : lo, m.distance ? hi - lo : hi);
      for (int i = 0; i < c; i++) by.get(m.id(at[i]))[0] += weights[l] * x[i];
    }
    return ranked(by);
  }

  /** The normalized values of v[0..c); for minmax, lo and hi of v (after a distance's flip). */
  private static double[] normalize(double[] v, int c, String how, double lo, double hi) {
    double[] x = new double[c];
    switch (how) {
      case "minmax" -> {
        for (int i = 0; i < c; i++) x[i] = hi > lo ? (v[i] - lo) / (hi - lo) : 1;
      }
      case "l2" -> {
        double ss = 0;
        for (int i = 0; i < c; i++) ss += v[i] * v[i];
        double norm = Math.sqrt(ss);
        for (int i = 0; i < c; i++) x[i] = norm > 0 ? v[i] / norm : 0;
      }
      default -> {  // zscore, dbsf: the list's mean and (population) standard deviation
        double mean = 0;
        for (int i = 0; i < c; i++) mean += v[i];
        mean /= c;
        double ss = 0;
        for (int i = 0; i < c; i++) ss += (v[i] - mean) * (v[i] - mean);
        double sd = Math.sqrt(ss / c);
        for (int i = 0; i < c; i++) {
          if (how.equals("zscore")) x[i] = sd > 0 ? (v[i] - mean) / sd : 0;
          else x[i] = sd > 0 ? Math.max(0, Math.min(1, (v[i] - (mean - 3 * sd)) / (6 * sd))) : 0.5;
        }
      }
    }
    return x;
  }

  /** {@link #blend(Matches[], double[])} for JavaScript or Python arrays. */
  public static Matches blend(Object lists, Object weights) {
    return blend(GuestVectors.matchesArray(lists), doubles(weights), "minmax");
  }

  /** {@link #blend(Matches[], double[], String)} for JavaScript or Python arrays. */
  public static Matches blend(Object lists, Object weights, String normalizer) {
    return blend(GuestVectors.matchesArray(lists), doubles(weights), normalizer);
  }

  /** Every id of the lists: {score 0, best rank}, in first-seen order. */
  private static java.util.Map<Long, double[]> merge(Matches[] lists) {
    if (lists == null) throw new IllegalArgumentException("the lists must not be null");
    java.util.Map<Long, double[]> by = new java.util.LinkedHashMap<>();
    for (int l = 0; l < lists.length; l++) {
      Matches m = lists[l];
      if (m == null) throw new IllegalArgumentException("list " + l + " is null");
      for (int i = 0; i < m.size(); i++) {
        double[] e = by.computeIfAbsent(m.id(i), x -> new double[] {0, Double.MAX_VALUE});
        e[1] = Math.min(e[1], i);
      }
    }
    return by;
  }

  private static Matches ranked(java.util.Map<Long, double[]> by) {
    java.util.List<java.util.Map.Entry<Long, double[]>> all = new java.util.ArrayList<>(by.entrySet());
    all.sort((a, b) -> {
      int c = Double.compare(b.getValue()[0], a.getValue()[0]);
      if (c != 0) return c;
      c = Double.compare(a.getValue()[1], b.getValue()[1]);
      return c != 0 ? c : Long.compare(a.getKey(), b.getKey());
    });
    int n = all.size();
    int[] index = new int[n];
    float[] score = new float[n];
    long[] id = new long[n];
    for (int i = 0; i < n; i++) {
      index[i] = i;
      id[i] = all.get(i).getKey();
      score[i] = (float) all.get(i).getValue()[0];
    }
    return new Matches(index, score, id, false);
  }

  /** How many rows matched (at most k). */
  public int size() {
    return index.length;
  }

  /** The i-th best row's position in the rows (or in the VectorSet). */
  public int index(int i) {
    return index[i];
  }

  /** The i-th best row's score. */
  public float score(int i) {
    return score[i];
  }

  /** The i-th best row's id (VectorSet), else its position. */
  public long id(int i) {
    return ids != null ? ids[i] : index[i];
  }

  /** All positions, best first (a copy). */
  public int[] indexes() {
    return index.clone();
  }

  /** All scores, best first (a copy). */
  public float[] scores() {
    return score.clone();
  }

  /** All ids, best first (a copy; positions when there are no ids). */
  public long[] ids() {
    long[] r = new long[index.length];
    for (int i = 0; i < r.length; i++) r[i] = id(i);
    return r;
  }

  /** JSON: [{"id": 7, "score": 0.93}, ...] (ids are positions when there are none). */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < index.length; i++) {
      if (i > 0) b.append(", ");
      b.append("{\"id\": ").append(id(i)).append(", \"score\": ");
      float s = score[i];
      if (Float.isFinite(s)) b.append(s);
      else b.append("null");
      b.append('}');
    }
    return b.append(']').toString();
  }
}
