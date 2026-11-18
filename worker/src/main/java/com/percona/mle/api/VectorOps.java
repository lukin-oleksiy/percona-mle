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

import static com.percona.mle.api.VectorEngine.COSINE;
import static com.percona.mle.api.VectorEngine.DOT;
import static com.percona.mle.api.VectorEngine.EUCLIDEAN;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * The vector jobs of exp note 53 (what people most often do with numpy on embeddings), shared by
 * {@link Vectors} (rows passed in) and {@link VectorSet} (rows kept in Java): aggregates,
 * threshold search, pairwise scores and matrices, MMR, similar pairs, groups, dedup, k-means. Not
 * API. The row scans are VectorEngine's kernels; results do not depend on the number of threads.
 */
final class VectorOps {
  private VectorOps() {}

  // -------------------------------------------------------------- aggregates

  /**
   * Sum or (weighted) mean of rows (which: positions, or null for all), summed in double in row
   * order. A weight of 0 leaves a row out.
   */
  static float[] combine(float[][] rows, int[] which, float[] weights, boolean mean) {
    int n = which == null ? rows.length : which.length;
    if (n == 0) throw new IllegalArgumentException("no rows to " + (mean ? "average" : "sum"));
    if (weights != null && weights.length != n)
      throw new IllegalArgumentException(n + " rows but " + weights.length + " weights");
    int d = rows[which == null ? 0 : which[0]].length;
    double[] acc = new double[d];
    double total = 0;
    for (int t = 0; t < n; t++) {
      if ((t & 4095) == 0) VectorEngine.stopIfKilled();
      float[] r = rows[which == null ? t : which[t]];
      double w = weights == null ? 1 : weights[t];
      if (w == 0) continue;
      for (int i = 0; i < d; i++) acc[i] += w * r[i];
      total += w;
    }
    if (mean && total == 0) throw new IllegalArgumentException("the weights sum to 0");
    float[] out = new float[d];
    for (int i = 0; i < d; i++) out[i] = (float) (mean ? acc[i] / total : acc[i]);
    return out;
  }

  // ---------------------------------------------------------- threshold search

  /** Rows with a score at least threshold (cosine, dot) or a distance at most threshold, best first. */
  static Matches within(float[] q, float[][] rows, float[] rt, int metric, double threshold, long[] ids) {
    float min = VectorEngine.keyOf(threshold, metric);
    float[] keys = VectorEngine.keys(q, rows, rt, metric);
    List<Integer> hit = new ArrayList<>();
    for (int j = 0; j < keys.length; j++)
      if (keys[j] >= min) hit.add(j);  // NaN never
    hit.sort((a, b) -> keys[a] != keys[b] ? Float.compare(keys[b], keys[a]) : Integer.compare(a, b));
    int[] index = new int[hit.size()];
    float[] score = new float[index.length];
    for (int i = 0; i < index.length; i++) {
      index[i] = hit.get(i);
      score[i] = VectorEngine.score(keys[index[i]], metric);
    }
    return matches(index, score, ids, metric);
  }

  static Matches matches(int[] index, float[] score, long[] ids, int metric) {
    long[] id = null;
    if (ids != null) {
      id = new long[index.length];
      for (int i = 0; i < id.length; i++) id[i] = ids[index[i]];
    }
    return new Matches(index, score, id, metric == EUCLIDEAN);
  }

  // ------------------------------------------------------ pairwise and matrix

  /** Row i of a with row i of b: one score each. */
  static float[] pairwise(float[][] a, float[][] b, int metric) {
    final int n = a.length;
    if (b.length != n) throw new IllegalArgumentException(n + " rows and " + b.length + " rows: pairwise needs as many");
    final float[] out = new float[n];
    final int parts = VectorEngine.parts((long) n * (n == 0 ? 0 : a[0].length), n);
    VectorEngine.run(parts, parts > 1, p -> {
      final float[] lanes = new float[VectorEngine.L];
      for (int i = (int) ((long) n * p / parts), hi = (int) ((long) n * (p + 1) / parts); i < hi; i++) {
        if ((i & 4095) == 0) VectorEngine.stopIfKilled();
        out[i] = switch (metric) {
          case EUCLIDEAN -> (float) Math.sqrt(VectorEngine.rowDistanceSquared(a[i], b[i], lanes));
          case DOT -> VectorEngine.rowDot(a[i], b[i], lanes);
          default -> VectorEngine.rowDot(a[i], b[i], lanes) * VectorEngine.inverseNorm(a[i], lanes)
              * VectorEngine.inverseNorm(b[i], lanes);
        };
      }
    });
    return out;
  }

  /** At most this many scores in one matrix (100 MB): beyond, nearest() keeps only the best. */
  static final long MATRIX_MAX = 25_000_000;

  /** Every row of a with every row of b: out[i][j]. */
  static float[][] similarity(float[][] a, float[][] b, int metric) {
    final int na = a.length, nb = b.length;
    if ((long) na * nb > MATRIX_MAX)
      throw new IllegalArgumentException(na + " x " + nb + " scores is more than " + MATRIX_MAX
          + ": use nearest() for the best k of each row, or pairs() for the pairs above a threshold");
    final float[][] out = new float[na][nb];
    if (na == 0 || nb == 0) return out;
    final float[] rt = VectorEngine.rowTerms(b, metric);
    final int parts = VectorEngine.parts((long) na * nb * b[0].length, na);
    VectorEngine.run(parts, parts > 1, p -> {
      final float[] lanes = new float[VectorEngine.L];
      for (int i = (int) ((long) na * p / parts), hi = (int) ((long) na * (p + 1) / parts); i < hi; i++) {
        float qInv = metric == COSINE ? VectorEngine.inverseNorm(a[i], lanes) : 0f;
        VectorEngine.keys(a[i], b, rt, metric, qInv, 0, nb, out[i]);
        if (metric == EUCLIDEAN)
          for (int j = 0; j < nb; j++) out[i][j] = VectorEngine.score(out[i][j], EUCLIDEAN);
      }
    });
    return out;
  }

  // --------------------------------------------------------------------- MMR

  /**
   * Maximal marginal relevance (Carbonell and Goldstein 1998), cosine: picks k of the candidates
   * one at a time, each maximizing lambda * sim(query, c) - (1 - lambda) * max sim(c, picked).
   * lambda 1: plain relevance; 0: as different from each other as possible. cand: positions in
   * rank order and rel their cosine with the query; null: every row. Scores in the result: the
   * cosine with the query, in pick order. Ties: the better-ranked candidate.
   */
  static Matches mmr(float[] q, float[][] rows, float[] rt, int k, double lambda, int[] cand, float[] rel,
      long[] ids) {
    if (!(lambda >= 0 && lambda <= 1)) throw new IllegalArgumentException("lambda must be in [0, 1], not " + lambda);
    VectorEngine.checkK(k);
    if (cand == null) {
      rel = VectorEngine.keys(q, rows, rt, COSINE);
      cand = new int[rows.length];
      for (int i = 0; i < cand.length; i++) cand[i] = i;
    }
    final int m = cand.length;
    final float[] lanes = new float[VectorEngine.L];
    final float[] inv = new float[m];
    for (int i = 0; i < m; i++) inv[i] = rt != null ? rt[cand[i]] : VectorEngine.inverseNorm(rows[cand[i]], lanes);
    final double[] maxSim = new double[m];
    Arrays.fill(maxSim, Double.NEGATIVE_INFINITY);
    final boolean[] used = new boolean[m];
    List<Integer> picked = new ArrayList<>();
    for (int step = 0; step < Math.min(k, m); step++) {
      VectorEngine.stopIfKilled();
      int best = -1;
      double bs = Double.NEGATIVE_INFINITY;
      for (int i = 0; i < m; i++) {
        if (used[i] || Float.isNaN(rel[i])) continue;
        double v = step == 0 ? rel[i] : lambda * rel[i] - (1 - lambda) * maxSim[i];
        if (v > bs || best < 0) {
          bs = v;
          best = i;
        }
      }
      if (best < 0) break;
      used[best] = true;
      picked.add(best);
      float[] b = rows[cand[best]];
      for (int i = 0; i < m; i++)
        if (!used[i]) maxSim[i] = Math.max(maxSim[i], VectorEngine.rowDot(b, rows[cand[i]], lanes) * inv[best] * inv[i]);
    }
    int[] index = new int[picked.size()];
    float[] score = new float[index.length];
    for (int i = 0; i < index.length; i++) {
      index[i] = cand[picked.get(i)];
      score[i] = rel[picked.get(i)];
    }
    return matches(index, score, ids, COSINE);
  }

  // ------------------------------------------------------- pairs and groups

  /** The best maxPairs pairs at least threshold similar (at most threshold apart), best first. */
  static Pairs pairs(float[][] rows, float[] rt, int metric, double threshold, int maxPairs, long[] ids) {
    if (maxPairs < 1) throw new IllegalArgumentException("maxPairs must be at least 1, not " + maxPairs);
    TopPairs top = new TopPairs(maxPairs);
    VectorEngine.pairs(rows, rt, metric, VectorEngine.keyOf(threshold, metric), top);
    top.prune(Math.min(top.size, maxPairs));
    int n = top.size;
    float[] score = new float[n];
    for (int i = 0; i < n; i++) score[i] = VectorEngine.score(top.key[i], metric);
    return new Pairs(Arrays.copyOf(top.a, n), Arrays.copyOf(top.b, n), score, ids, metric == EUCLIDEAN);
  }

  /** Keeps the best pairs: key descending, then a, then b (a total order: thread independent). */
  private static final class TopPairs implements VectorEngine.PairSink {
    final int max;
    int[] a = new int[1024], b = new int[1024];
    float[] key = new float[1024];
    int size;
    float cut = Float.NEGATIVE_INFINITY;

    TopPairs(int max) {
      this.max = max;
    }

    @Override
    public float take(int[] pa, int[] pb, float[] pk, int count) {
      if (size + count > a.length) {
        int cap = Math.max(a.length * 2, size + count);
        a = Arrays.copyOf(a, cap);
        b = Arrays.copyOf(b, cap);
        key = Arrays.copyOf(key, cap);
      }
      for (int i = 0; i < count; i++)
        if (pk[i] >= cut) {
          a[size] = pa[i];
          b[size] = pb[i];
          key[size++] = pk[i];
        }
      if (size >= 2 * max) {
        prune(max);
        cut = key[max - 1];
      }
      return cut;
    }

    /** Sorts and keeps the best keep pairs. */
    void prune(int keep) {
      Integer[] o = new Integer[size];
      for (int i = 0; i < size; i++) o[i] = i;
      Arrays.sort(o, (x, y) -> key[x] != key[y] ? Float.compare(key[y], key[x])
          : a[x] != a[y] ? Integer.compare(a[x], a[y]) : Integer.compare(b[x], b[y]));
      int[] na = new int[Math.max(keep, 1024)], nb = new int[na.length];
      float[] nk = new float[na.length];
      for (int i = 0; i < keep; i++) {
        na[i] = a[o[i]];
        nb[i] = b[o[i]];
        nk[i] = key[o[i]];
      }
      a = na;
      b = nb;
      key = nk;
      size = keep;
    }
  }

  /** Each row's component root (the lowest row of its group) in the "at least threshold similar" graph. */
  static int[] components(float[][] rows, float[] rt, int metric, double threshold) {
    final int n = rows.length;
    final int[] parent = new int[n];
    for (int i = 0; i < n; i++) parent[i] = i;
    VectorEngine.pairs(rows, rt, metric, VectorEngine.keyOf(threshold, metric), (pa, pb, pk, count) -> {
      for (int i = 0; i < count; i++) {
        int x = find(parent, pa[i]), y = find(parent, pb[i]);
        if (x < y) parent[y] = x;  // the lower row is the root: the result is the same in any order
        else if (y < x) parent[x] = y;
      }
      return Float.NEGATIVE_INFINITY;
    });
    for (int i = 0; i < n; i++) parent[i] = find(parent, i);
    return parent;
  }

  private static int find(int[] parent, int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }

  static Groups groups(float[][] rows, float[] rt, int metric, double threshold, int minSize, long[] ids) {
    if (minSize < 1) throw new IllegalArgumentException("minSize must be at least 1, not " + minSize);
    int[] root = components(rows, rt, metric, threshold);
    int n = root.length;
    int[] size = new int[n];
    for (int r : root) size[r]++;
    List<int[]> out = new ArrayList<>();
    int[][] byRoot = new int[n][];
    int[] fill = new int[n];
    for (int i = 0; i < n; i++) {
      int r = root[i];
      if (size[r] < minSize) continue;
      if (byRoot[r] == null) {
        byRoot[r] = new int[size[r]];
        out.add(byRoot[r]);
      }
      byRoot[r][fill[r]++] = i;
    }
    out.sort((x, y) -> x.length != y.length ? Integer.compare(y.length, x.length) : Integer.compare(x[0], y[0]));
    return new Groups(out.toArray(new int[0][]), ids);
  }

  /** The ids (positions when none) of the rows to keep: the first row of each group, in row order. */
  static long[] dedup(float[][] rows, float[] rt, int metric, double threshold, long[] ids) {
    int[] root = components(rows, rt, metric, threshold);
    int m = 0;
    for (int i = 0; i < root.length; i++)
      if (root[i] == i) m++;
    long[] keep = new long[m];
    for (int i = 0, o = 0; i < root.length; i++)
      if (root[i] == i) keep[o++] = ids != null ? ids[i] : i;
    return keep;
  }

  // ----------------------------------------------------------------- k-means

  /**
   * k-means: greedy k-means++ seeding (Arthur and Vassilvitskii 2007) with java.util.Random(seed), then
   * Lloyd iterations until no row changes cluster or maxIterations updates. 'cosine': spherical
   * k-means (unit centroids, rows weighted by 1/|row|). An empty cluster restarts at the row
   * farthest from its centroid. The assignment is the batch nearest() kernel (rows as queries,
   * centroids as rows); the update sums strips of dimensions in row order (thread independent).
   */
  static Clusters kmeans(float[][] rows, float[] rt, long[] ids, int k, int metric, int maxIterations, long seed) {
    if (metric == DOT) throw new IllegalArgumentException("k-means needs 'euclidean' or 'cosine', not 'dot'");
    final int n = rows.length;
    if (k < 1 || k > n) throw new IllegalArgumentException("k must be in [1, " + n + "] (the rows), not " + k);
    if (maxIterations < 1) throw new IllegalArgumentException("maxIterations must be at least 1, not " + maxIterations);
    final int d = rows[0].length;
    for (int j = 0; j < n; j++)
      for (float x : rows[j])
        if (!Float.isFinite(x)) throw new IllegalArgumentException("row " + j + " has a NaN or infinite value");
    final float[] inv = metric == COSINE ? (rt != null ? rt : VectorEngine.rowTerms(rows, COSINE)) : null;
    final Random rnd = new Random(seed);
    final float[][] cent = new float[k][];
    // Greedy k-means++ (as scikit-learn): 2 + ln k candidates per centre, sampled by D², and the one
    // that lowers the potential (the sum of D²) most is kept; plain k-means++ puts two centres in
    // one well-separated cluster a few percent of the time.
    final int trials = 2 + (int) Math.log(k);
    int first = rnd.nextInt(n);
    cent[0] = center(rows[first], inv == null ? 1f : inv[first]);
    double[] dist2 = new double[n], cand = new double[n], best = new double[n];
    distances(rows, inv, cent[0], metric, dist2);
    for (int c = 1; c < k; c++) {
      double total = 0;
      for (double x : dist2) total += x;
      int chosen = -1;
      double bestPotential = Double.POSITIVE_INFINITY;
      for (int t = 0; t < trials; t++) {
        int p = total > 0 ? sample(dist2, total, rnd) : rnd.nextInt(n);
        distances(rows, inv, center(rows[p], inv == null ? 1f : inv[p]), metric, cand);
        double potential = 0;
        for (int j = 0; j < n; j++) potential += Math.min(dist2[j], cand[j]);
        if (chosen < 0 || potential < bestPotential) {
          bestPotential = potential;
          chosen = p;
          double[] x = best;
          best = cand;
          cand = x;
        }
      }
      cent[c] = center(rows[chosen], inv == null ? 1f : inv[chosen]);
      for (int j = 0; j < n; j++) dist2[j] = Math.min(dist2[j], best[j]);
    }
    final int[] assign = new int[n];
    Arrays.fill(assign, -1);
    final float[] score = new float[n];
    int updates = 0;
    final boolean parallel = (long) n * d >= VectorEngine.PARALLEL_MIN;
    while (true) {
      Matches[] m = VectorEngine.nearest(rows, cent, null, 1, metric, false, null);
      int changed = 0;
      for (int j = 0; j < n; j++) {
        int c = m[j].index(0);
        if (c != assign[j]) changed++;
        assign[j] = c;
        score[j] = m[j].score(0);
      }
      if (changed == 0 || updates == maxIterations) break;
      final int[] count = new int[k];
      for (int c : assign) count[c]++;
      final double[][] sums = new double[k][d];
      final int strips = (d + 63) / 64;
      VectorEngine.run(strips, parallel, s -> {
        final int lo = s * 64, hi = Math.min(d, lo + 64);
        for (int j = 0; j < n; j++) {
          if ((j & 4095) == 0) VectorEngine.stopIfKilled();
          final double w = inv == null ? 1 : inv[j];
          final double[] acc = sums[assign[j]];
          final float[] r = rows[j];
          for (int i = lo; i < hi; i++) acc[i] += w * r[i];
        }
      });
      boolean[] taken = new boolean[n];
      for (int c = 0; c < k; c++) {
        if (count[c] > 0) {
          float[] v = new float[d];
          double norm = 0;
          for (int i = 0; i < d; i++) {
            double x = inv == null ? sums[c][i] / count[c] : sums[c][i];
            v[i] = (float) x;
            norm += x * x;
          }
          if (inv != null && norm > 0) {
            float s = (float) (1 / Math.sqrt(norm));
            for (int i = 0; i < d; i++) v[i] *= s;
          }
          cent[c] = v;
        } else {  // empty: restart at the worst-fitting row of a cluster with others in it
          int worst = -1;
          for (int j = 0; j < n; j++) {
            if (taken[j] || count[assign[j]] < 2) continue;
            if (worst < 0 || (metric == EUCLIDEAN ? score[j] > score[worst] : score[j] < score[worst])) worst = j;
          }
          if (worst < 0) worst = 0;
          taken[worst] = true;
          count[assign[worst]]--;
          cent[c] = center(rows[worst], inv == null ? 1f : inv[worst]);
        }
      }
      updates++;
    }
    double inertia = 0;
    for (int j = 0; j < n; j++) inertia += metric == EUCLIDEAN ? (double) score[j] * score[j] : 1 - score[j];
    return new Clusters(cent, assign, score, ids, metric, updates, inertia);
  }

  /** A copy of row scaled by s (1/|row| for cosine: a unit centroid). */
  private static float[] center(float[] row, float s) {
    float[] c = row.clone();
    if (s != 1f)
      for (int i = 0; i < c.length; i++) c[i] *= s;
    return c;
  }

  /** A row drawn with probability dist2[j] / total. */
  private static int sample(double[] dist2, double total, Random rnd) {
    double r = rnd.nextDouble() * total, cum = 0;
    int pick = -1;
    for (int j = 0; j < dist2.length; j++) {
      if (dist2[j] <= 0) continue;
      cum += dist2[j];
      pick = j;
      if (cum > r) break;
    }
    return pick;
  }

  /** out[j] = D(row j, c)²: Euclidean, or 1 - cosine for 'cosine' (c of unit length). */
  private static void distances(float[][] rows, float[] inv, float[] c, int metric, double[] out) {
    final int n = rows.length;
    final int parts = VectorEngine.parts((long) n * c.length, n);
    final int tasks = Math.max(parts, (n + 32767) / 32768);
    VectorEngine.run(tasks, parts > 1, p -> {
      final float[] lanes = new float[VectorEngine.L];
      for (int j = (int) ((long) n * p / tasks), hi = (int) ((long) n * (p + 1) / tasks); j < hi; j++) {
        if (metric == EUCLIDEAN) {
          out[j] = VectorEngine.rowDistanceSquared(rows[j], c, lanes);
        } else {
          double dd = Math.max(0, 1 - (double) VectorEngine.rowDot(rows[j], c, lanes) * inv[j]);
          out[j] = dd * dd;
        }
      }
    });
  }
}
