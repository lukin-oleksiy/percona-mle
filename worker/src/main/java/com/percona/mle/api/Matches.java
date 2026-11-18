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
   * the lists it is in (rank from 1), by id; only ranks count, so lists with scores on different
   * scales (cosine, full-text relevance, distance) combine. Best first; ties: the better best rank,
   * then the lower id. A small k favours the top ranks, a large one spreads the weight.
   */
  public static Matches fuse(int k, Matches... lists) {
    if (k < 0) throw new IllegalArgumentException("k must not be negative, not " + k);
    java.util.Map<Long, double[]> by = merge(lists);  // id -> {score, best rank}
    for (Matches m : lists)
      for (int i = 0; i < m.size(); i++) {
        double[] e = by.get(m.id(i));
        e[0] += 1.0 / (k + i + 1);
      }
    return ranked(by);
  }

  /** {@link #fuse(int, Matches...)} for a JavaScript or Python array of Matches. */
  public static Matches fuse(int k, Object lists) {
    return fuse(k, GuestVectors.matchesArray(lists));
  }

  /**
   * Weighted sum of normalized scores, by id: each list's scores are scaled to [0, 1] (min-max;
   * distances flipped, so 1 is the best of the list; all equal: 1), missing ids count 0. Best first;
   * ties as in {@link #fuse}. weights: one per list.
   */
  public static Matches blend(Matches[] lists, double[] weights) {
    if (lists == null || weights == null) throw new IllegalArgumentException("lists and weights must not be null");
    if (lists.length != weights.length)
      throw new IllegalArgumentException(lists.length + " lists but " + weights.length + " weights");
    java.util.Map<Long, double[]> by = merge(lists);
    for (int l = 0; l < lists.length; l++) {
      Matches m = lists[l];
      double lo = Double.POSITIVE_INFINITY, hi = Double.NEGATIVE_INFINITY;
      for (float s : m.score)
        if (Float.isFinite(s)) {
          lo = Math.min(lo, s);
          hi = Math.max(hi, s);
        }
      for (int i = 0; i < m.size(); i++) {
        float s = m.score[i];
        if (!Float.isFinite(s)) continue;
        double x = hi > lo ? (m.distance ? hi - s : s - lo) / (hi - lo) : 1;
        by.get(m.id(i))[0] += weights[l] * x;
      }
    }
    return ranked(by);
  }

  /** {@link #blend(Matches[], double[])} for JavaScript or Python arrays. */
  public static Matches blend(Object lists, Object weights) {
    float[] w = GuestVectors.vector(weights, "the weights");
    double[] d = new double[w.length];
    for (int i = 0; i < w.length; i++) d[i] = w[i];
    return blend(GuestVectors.matchesArray(lists), d);
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
