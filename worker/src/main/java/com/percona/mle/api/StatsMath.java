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

import java.util.Arrays;

/**
 * The numerics of the statistics helper (exp note 54). Not API. NaN is a missing value and is left
 * out (pandas' skipna); pairs leave out a pair with a NaN on either side. Sums in double, two-pass
 * moments; quantiles by linear interpolation (numpy's and pandas' default); p-values from the
 * regularized incomplete beta function (continued fraction, Lentz) and a Lanczos log-gamma.
 */
final class StatsMath {
  private StatsMath() {}

  // ------------------------------------------------------------ one column

  /** The values that are not NaN, sorted. */
  static double[] sorted(double[] x) {
    int n = 0;
    for (double v : x)
      if (!Double.isNaN(v)) n++;
    double[] s = new double[n];
    for (int i = 0, o = 0; i < x.length; i++)
      if (!Double.isNaN(x[i])) s[o++] = x[i];
    Arrays.sort(s);
    return s;
  }

  static void checkQ(double q) {
    if (!(q >= 0 && q <= 1)) throw new IllegalArgumentException("a quantile must be in [0, 1], not " + q);
  }

  /** Quantile q of sorted values, linear interpolation between the closest ranks; NaN when empty. */
  static double quantileSorted(double[] s, double q) {
    checkQ(q);
    int n = s.length;
    if (n == 0) return Double.NaN;
    double h = (n - 1) * q;
    int lo = (int) Math.floor(h), hi = Math.min(lo + 1, n - 1);
    double g = h - lo;
    if (g == 0) return s[lo];
    double d = s[hi] - s[lo];
    // The difference of two finite values of opposite sign can overflow: weigh the two instead.
    if (Double.isInfinite(d) && !Double.isInfinite(s[lo]) && !Double.isInfinite(s[hi]))
      return (1 - g) * s[lo] + g * s[hi];
    return s[lo] + g * d;
  }

  static double mean(double[] s) {
    if (s.length == 0) return Double.NaN;
    double sum = 0;
    for (double v : s) sum += v;
    double m = sum / s.length;
    if (Double.isFinite(m)) {  // a second pass corrects the rounding of the first
      double c = 0;
      for (double v : s) c += v - m;
      m += c / s.length;
    }
    return m;
  }

  /** {sum (x - m)^2, ^3, ^4} around m. */
  static double[] centralSums(double[] s, double m) {
    double s2 = 0, s3 = 0, s4 = 0;
    for (double v : s) {
      double d = v - m, d2 = d * d;
      s2 += d2;
      s3 += d2 * d;
      s4 += d2 * d2;
    }
    return new double[] {s2, s3, s4};
  }

  /** The most frequent value of sorted values; a tie goes to the smallest (scipy.stats.mode). */
  static double modeSorted(double[] s) {
    if (s.length == 0) return Double.NaN;
    double best = s[0];
    int bestRun = 0;
    for (int i = 0; i < s.length; ) {
      int j = i;
      while (j < s.length && s[j] == s[i]) j++;
      if (j - i > bestRun) {
        bestRun = j - i;
        best = s[i];
      }
      i = j;
    }
    return best;
  }

  /** Ranks from 1, ties get their average rank (scipy.stats.rankdata 'average'); NaN stays NaN. */
  static double[] rank(double[] x) {
    int n = 0;
    for (double v : x)
      if (!Double.isNaN(v)) n++;
    int[] idx = new int[n];
    for (int i = 0, o = 0; i < x.length; i++)
      if (!Double.isNaN(x[i])) idx[o++] = i;
    sortIndexes(idx, x);
    double[] r = new double[x.length];
    Arrays.fill(r, Double.NaN);
    for (int i = 0; i < n; ) {
      int j = i;
      while (j < n && x[idx[j]] == x[idx[i]]) j++;
      double avg = (i + 1 + j) / 2.0;  // ranks i+1 .. j
      for (int t = i; t < j; t++) r[idx[t]] = avg;
      i = j;
    }
    return r;
  }

  /** Sorts positions by their values (stable merge sort, no boxing). */
  static void sortIndexes(int[] idx, double[] key) {
    int[] tmp = new int[idx.length];
    for (int width = 1; width < idx.length; width *= 2) {
      for (int lo = 0; lo < idx.length; lo += 2 * width) {
        int mid = Math.min(lo + width, idx.length), hi = Math.min(lo + 2 * width, idx.length);
        int a = lo, b = mid, o = lo;
        while (a < mid && b < hi) tmp[o++] = Double.compare(key[idx[b]], key[idx[a]]) < 0 ? idx[b++] : idx[a++];
        while (a < mid) tmp[o++] = idx[a++];
        while (b < hi) tmp[o++] = idx[b++];
      }
      System.arraycopy(tmp, 0, idx, 0, idx.length);
    }
  }

  // ---------------------------------------------------------------- pairs

  /** The pairs (x[i], y[i]) without a NaN: {xs, ys}. */
  static double[][] complete(double[] x, double[] y) {
    if (x.length != y.length)
      throw new IllegalArgumentException("the columns have " + x.length + " and " + y.length + " values");
    int n = 0;
    for (int i = 0; i < x.length; i++)
      if (!Double.isNaN(x[i]) && !Double.isNaN(y[i])) n++;
    double[] a = new double[n], b = new double[n];
    for (int i = 0, o = 0; i < x.length; i++)
      if (!Double.isNaN(x[i]) && !Double.isNaN(y[i])) {
        a[o] = x[i];
        b[o++] = y[i];
      }
    return new double[][] {a, b};
  }

  /** {sum dx², sum dy², sum dx dy} around the means, for complete pairs. */
  static double[] comoments(double[] a, double[] b) {
    double ma = mean(a), mb = mean(b), sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < a.length; i++) {
      double dx = a[i] - ma, dy = b[i] - mb;
      sxx += dx * dx;
      syy += dy * dy;
      sxy += dx * dy;
    }
    return new double[] {sxx, syy, sxy, ma, mb};
  }

  static double pearson(double[] a, double[] b) {
    if (a.length < 2) return Double.NaN;
    double[] m = comoments(a, b);
    if (m[0] == 0 || m[1] == 0) return Double.NaN;
    return r(m[0], m[1], m[2]);
  }

  /**
   * Pearson's r = sxy / sqrt(sxx syy), kept in [-1, 1]. The product sxx syy overflows or underflows
   * double for values around 1e77 (1e-77) and beyond although r does not, so the two sums are
   * first scaled by powers of 4 (exact: other data gives the same bits as the plain formula).
   */
  static double r(double sxx, double syy, double sxy) {
    int ex = Math.getExponent(sxx), ey = Math.getExponent(syy);
    ex -= ex & 1;  // even, so that the square root's scale is a power of 2
    ey -= ey & 1;
    double p = Math.scalb(sxx, -ex) * Math.scalb(syy, -ey);
    return Math.max(-1, Math.min(1, Math.scalb(sxy, -(ex + ey) / 2) / Math.sqrt(p)));
  }

  static double correlation(double[] x, double[] y, String method) {
    double[][] p = complete(x, y);
    return switch (method == null ? "" : method.trim().toLowerCase(java.util.Locale.ROOT)) {
      case "pearson" -> pearson(p[0], p[1]);
      case "spearman" -> pearson(rank(p[0]), rank(p[1]));
      default -> throw new IllegalArgumentException("unknown method '" + method + "' (pearson or spearman)");
    };
  }

  // ---------------------------------------------------------- distributions

  private static final double[] LANCZOS = {0.99999999999980993, 676.5203681218851, -1259.1392167224028,
      771.32342877765313, -176.61502916214059, 12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6,
      1.5056327351493116e-7};

  /** ln Γ(x) for x > 0 (Lanczos, g = 7). */
  static double lgamma(double x) {
    if (x < 0.5) return Math.log(Math.PI / Math.abs(Math.sin(Math.PI * x))) - lgamma(1 - x);
    x -= 1;
    double a = LANCZOS[0], t = x + 7.5;
    for (int i = 1; i < 9; i++) a += LANCZOS[i] / (x + i);
    return 0.5 * Math.log(2 * Math.PI) + (x + 0.5) * Math.log(t) - t + Math.log(a);
  }

  /** The regularized incomplete beta function I_x(a, b). */
  static double betai(double a, double b, double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double front = Math.exp(lgamma(a + b) - lgamma(a) - lgamma(b) + a * Math.log(x) + b * Math.log1p(-x));
    if (x < (a + 1) / (a + b + 2)) return front * betacf(a, b, x) / a;
    return 1 - front * betacf(b, a, 1 - x) / b;
  }

  /** The continued fraction of the incomplete beta function (modified Lentz). */
  private static double betacf(double a, double b, double x) {
    final double tiny = 1e-300, eps = 1e-16;
    double qab = a + b, qap = a + 1, qam = a - 1, c = 1, d = 1 - qab * x / qap;
    if (Math.abs(d) < tiny) d = tiny;
    d = 1 / d;
    double h = d;
    for (int m = 1; m <= 1000; m++) {
      int m2 = 2 * m;
      double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
      d = 1 + aa * d;
      if (Math.abs(d) < tiny) d = tiny;
      c = 1 + aa / c;
      if (Math.abs(c) < tiny) c = tiny;
      d = 1 / d;
      h *= d * c;
      aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
      d = 1 + aa * d;
      if (Math.abs(d) < tiny) d = tiny;
      c = 1 + aa / c;
      if (Math.abs(c) < tiny) c = tiny;
      d = 1 / d;
      double del = d * c;
      h *= del;
      if (Math.abs(del - 1) < eps) break;
    }
    return h;
  }

  /** P(T > t) for Student's t with df degrees of freedom. */
  static double tSf(double t, double df) {
    if (Double.isNaN(t) || !(df > 0)) return Double.NaN;
    if (Double.isInfinite(t)) return t > 0 ? 0 : 1;
    double p = 0.5 * betai(df / 2, 0.5, df / (df + t * t));
    return t > 0 ? p : 1 - p;
  }
}
