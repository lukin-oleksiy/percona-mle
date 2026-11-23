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

package e2e;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Arrays;

/**
 * The re-ranking of the end-to-end benchmark (perf/e2e), once in Java: a library of MLE routines (CREATE LIBRARY
 * e2e_rerank LANGUAGE JAVA, used by the Java procedures of setup.sql) and the same source compiled into the
 * external Java application (E2E).
 *
 * <p>Score of a candidate: 0.8 x similarity (1 - cosine distance) + 0.1 x cheapness (1 - price / 1000) + 0.05 if in
 * stock + 0.05 if in the preferred category. P1: the top k by score (ties: smaller id). P2: maximal marginal
 * relevance, lambda 0.7: each step picks the candidate with the best lambda x score - (1 - lambda) x its highest
 * similarity to the ones already picked (vectors are unit length, so similarity is the dot product); ties: the
 * earlier candidate (nearer by distance).
 */
public final class Rerank {
  private Rerank() {}

  public static final double LAMBDA = 0.7;

  public static double[] scores(double[] dist, double[] price, long[] stock, long[] category, long preferred) {
    double[] s = new double[dist.length];
    for (int i = 0; i < s.length; i++)
      s[i] = 0.8 * (1 - dist[i]) + 0.1 * (1 - price[i] / 1000) + (stock[i] > 0 ? 0.05 : 0)
          + (category[i] == preferred ? 0.05 : 0);
    return s;
  }

  /** P1: the ids of the k best scores. */
  public static long[] top(long[] ids, double[] s, int k) {
    Integer[] order = new Integer[ids.length];
    for (int i = 0; i < order.length; i++) order[i] = i;
    Arrays.sort(order, (a, b) -> s[a] != s[b] ? Double.compare(s[b], s[a]) : Long.compare(ids[a], ids[b]));
    long[] out = new long[Math.min(k, ids.length)];
    for (int i = 0; i < out.length; i++) out[i] = ids[order[i]];
    return out;
  }

  /** P2: the ids picked by maximal marginal relevance; v[i] is candidate i's unit vector. */
  public static long[] mmr(long[] ids, double[] s, float[][] v, int k) {
    int n = ids.length;
    double[] maxSim = new double[n];
    boolean[] taken = new boolean[n];
    long[] out = new long[Math.min(k, n)];
    for (int step = 0; step < out.length; step++) {
      int best = -1;
      double bestValue = Double.NEGATIVE_INFINITY;
      for (int i = 0; i < n; i++) {
        if (taken[i]) continue;
        double value = LAMBDA * s[i] - (step == 0 ? 0 : (1 - LAMBDA) * maxSim[i]);
        if (value > bestValue) {
          bestValue = value;
          best = i;
        }
      }
      taken[best] = true;
      out[step] = ids[best];
      float[] picked = v[best];
      for (int i = 0; i < n; i++) {
        double d = dot(v[i], picked);
        maxSim[i] = step == 0 ? d : Math.max(maxSim[i], d);
      }
    }
    return out;
  }

  // ---------------------------------------------------------------- P3: personalised recommendations

  /**
   * P3 first stage: 0.8 x similarity + 0.05 each for the preferred category, a price within the limit and stock;
   * items the user has viewed get -infinity (excluded).
   */
  public static double[] personalScores(long[] ids, double[] dist, double[] price, long[] stock, long[] category,
      long preferred, double maxPrice, long[] viewed) {
    java.util.Set<Long> seen = new java.util.HashSet<>();
    for (long v : viewed) seen.add(v);
    double[] s = new double[ids.length];
    for (int i = 0; i < s.length; i++)
      s[i] = seen.contains(ids[i]) ? Double.NEGATIVE_INFINITY
          : 0.8 * (1 - dist[i]) + (category[i] == preferred ? 0.05 : 0) + (price[i] <= maxPrice ? 0.05 : 0)
              + (stock[i] > 0 ? 0.05 : 0);
    return s;
  }

  /** P3 final stage: the shortlist's scores plus 0.2 x the promotion's discount (0 without one); the top k. */
  public static long[] promoted(long[] shortlist, double[] scores, double[] discount, int k) {
    double[] s = new double[shortlist.length];
    for (int i = 0; i < s.length; i++) s[i] = scores[i] + 0.2 * discount[i];
    return top(shortlist, s, k);
  }

  /** The user vector: the mean of the history's vectors, normalised (in double, then float). */
  public static float[] mean(float[][] v) {
    int d = v[0].length;
    double[] m = new double[d];
    for (float[] x : v)
      for (int j = 0; j < d; j++) m[j] += x[j];
    double norm = 0;
    for (int j = 0; j < d; j++) norm += m[j] * m[j];
    norm = Math.sqrt(norm);
    float[] out = new float[d];
    for (int j = 0; j < d; j++) out[j] = (float) (m[j] / norm);
    return out;
  }

  // ---------------------------------------------------------------- P4: bulk analysis

  /**
   * P4: the k items most typical of a set (highest similarity to its normalised centroid) followed by the k
   * outliers (lowest); ties: smaller id. In double, as numpy's float64 centroid.
   */
  public static long[] typical(long[] ids, float[][] v, int k) {
    int n = ids.length, d = v[0].length;
    double[] c = new double[d];
    for (float[] x : v)
      for (int j = 0; j < d; j++) c[j] += x[j];
    double norm = 0;
    for (int j = 0; j < d; j++) norm += (c[j] / n) * (c[j] / n);
    norm = Math.sqrt(norm);
    for (int j = 0; j < d; j++) c[j] = c[j] / n / norm;
    double[] sim = new double[n];
    for (int i = 0; i < n; i++) {
      double acc = 0;
      float[] x = v[i];
      for (int j = 0; j < d; j++) acc += x[j] * c[j];
      sim[i] = acc;
    }
    long[] most = top(ids, sim, k);
    double[] neg = new double[n];
    for (int i = 0; i < n; i++) neg[i] = -sim[i];
    long[] least = top(ids, neg, k);
    long[] out = new long[most.length + least.length];
    System.arraycopy(most, 0, out, 0, most.length);
    System.arraycopy(least, 0, out, most.length, least.length);
    return out;
  }

  // ---------------------------------------------------------------- P5/P6: enrich and write back

  /**
   * P5/P6: each item's similarity to the set's normalised centroid (returned), and, when out is given, the item's
   * vector minus the centroid, normalised (into out[i]). In double, as numpy's float64.
   */
  public static double[] enrich(float[][] v, float[][] out) {
    int n = v.length, d = v[0].length;
    double[] c = new double[d];
    for (float[] x : v)
      for (int j = 0; j < d; j++) c[j] += x[j];
    double norm = 0;
    for (int j = 0; j < d; j++) norm += (c[j] / n) * (c[j] / n);
    norm = Math.sqrt(norm);
    for (int j = 0; j < d; j++) c[j] = c[j] / n / norm;
    double[] sim = new double[n];
    double[] w = out != null ? new double[d] : null;
    for (int i = 0; i < n; i++) {
      float[] x = v[i];
      double acc = 0;
      for (int j = 0; j < d; j++) acc += x[j] * c[j];
      sim[i] = acc;
      if (out != null) {
        double nn = 0;
        for (int j = 0; j < d; j++) {
          w[j] = x[j] - c[j];
          nn += w[j] * w[j];
        }
        nn = Math.sqrt(nn);
        float[] o = new float[d];
        for (int j = 0; j < d; j++) o[j] = (float) (w[j] / nn);
        out[i] = o;
      }
    }
    return sim;
  }

  /**
   * The dot product, chosen once: MLE's SIMD vector helper (com.percona.mle.api.Vectors.dot) inside MLE, the
   * external application's Vector API version (e2e.SimdDot) outside, else a plain loop. The same SIMD speed on both
   * sides: the comparison measures where the code runs, not how well it is written.
   */
  static final MethodHandle DOT = pickDot();

  private static MethodHandle pickDot() {
    MethodType t = MethodType.methodType(float.class, float[].class, float[].class);
    for (String c : new String[] {"com.percona.mle.api.Vectors", "e2e.SimdDot"}) {
      try {
        return MethodHandles.publicLookup().findStatic(Class.forName(c), "dot", t);
      } catch (ReflectiveOperationException | LinkageError e) {
        // not here
      }
    }
    return null;
  }

  static double dot(float[] a, float[] b) {
    if (DOT != null) {
      try {
        return (float) DOT.invokeExact(a, b);
      } catch (Throwable e) {
        throw new IllegalStateException(e);
      }
    }
    float acc = 0;
    for (int i = 0; i < a.length; i++) acc += a[i] * b[i];
    return acc;
  }

  /** Which dot product is used (for the results). */
  public static String dotKind() {
    if (DOT == null) return "scalar";
    try {
      Class.forName("com.percona.mle.api.Vectors");
      return "mle-vectors";
    } catch (ClassNotFoundException e) {
      return "vector-api";
    }
  }
}
