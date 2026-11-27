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
import java.util.Locale;

/**
 * Rolling statistics over a trailing window of rows ({@link Stats#rolling}), as pandas'
 * {@code Series.rolling(window, min_periods).<stat>()}: output i covers values i - window + 1 to i;
 * a missing value (NaN) is left out of its windows; a window with fewer than minPeriods values
 * gives NaN. Not API.
 *
 * <p>Threads: the outputs are cut into blocks of {@link #BLOCK}, each computed from scratch (its
 * window preloaded), on the vector pool when the work is large. The cut does not depend on the
 * number of threads, so neither do the results. mean, sum, var and std update running sums
 * (Neumaier, Welford) within a block; min and max a monotonic queue; median, quantile and mad a
 * sorted window (O(window) per row).
 */
final class StatsWindows {
  private StatsWindows() {}

  static final int BLOCK = 4096;

  private enum Kind { MEAN, SUM, VAR, STD, MIN, MAX, QUANTILE, MAD }

  static double[] rolling(double[] x, int window, String stat, int minPeriods) {
    if (x == null) throw new IllegalArgumentException("the column must not be null");
    if (window < 1) throw new IllegalArgumentException("the window must be at least 1 row, not " + window);
    if (minPeriods < 1 || minPeriods > window)
      throw new IllegalArgumentException("minPeriods must be from 1 to the window (" + window + "), not " + minPeriods);
    String s = stat == null ? "" : stat.trim().toLowerCase(Locale.ROOT);
    double q = 0.5;
    Kind kind;
    if (s.startsWith("quantile:")) {
      try {
        q = Double.parseDouble(s.substring(9).trim());
      } catch (NumberFormatException e) {
        throw new IllegalArgumentException("'" + stat + "': the quantile must be a number, as quantile:0.9");
      }
      StatsMath.checkQ(q);
      kind = Kind.QUANTILE;
    } else {
      kind = switch (s) {
        case "mean" -> Kind.MEAN;
        case "sum" -> Kind.SUM;
        case "var" -> Kind.VAR;
        case "std" -> Kind.STD;
        case "min" -> Kind.MIN;
        case "max" -> Kind.MAX;
        case "median" -> Kind.QUANTILE;
        case "mad" -> Kind.MAD;
        default -> throw new IllegalArgumentException("unknown statistic '" + stat
            + "' (mean, sum, var, std, min, max, median, quantile:q, mad)");
      };
    }
    int n = x.length;
    double[] out = new double[n];
    int blocks = (n + BLOCK - 1) / BLOCK;
    boolean sorted = kind == Kind.QUANTILE || kind == Kind.MAD;
    long work = (long) n * (sorted ? Math.min(window, n) : 4);
    boolean parallel = VectorEngine.parts(work, blocks) > 1;
    final Kind k = kind;
    final double qq = q;
    VectorEngine.run(blocks, parallel, b -> {
      int lo = b * BLOCK, hi = Math.min(n, lo + BLOCK);
      switch (k) {
        case MEAN, SUM, VAR, STD -> moments(x, window, minPeriods, k, lo, hi, out);
        case MIN, MAX -> extreme(x, window, minPeriods, k == Kind.MAX, lo, hi, out);
        default -> ordered(x, window, minPeriods, k == Kind.MAD, qq, lo, hi, out);
      }
    });
    return out;
  }

  private static void moments(double[] x, int w, int minP, Kind k, int lo, int hi, double[] out) {
    long count = 0;
    double sum = 0, comp = 0, mean = 0, m2 = 0;
    for (int i = Math.max(0, lo - w + 1), first = i; i < hi; i++) {
      double v = x[i];
      if (!Double.isNaN(v)) {  // add v
        count++;
        double t = sum + v;  // Neumaier
        comp += Math.abs(sum) >= Math.abs(v) ? (sum - t) + v : (v - t) + sum;
        sum = t;
        double d = v - mean;
        mean += d / count;
        m2 += d * (v - mean);
      }
      int old = i - w;
      if (old >= first && !Double.isNaN(x[old])) {  // remove x[old]
        double u = x[old];
        count--;
        double t = sum - u;
        comp += Math.abs(sum) >= Math.abs(u) ? (sum - t) - u : (-u - t) + sum;
        sum = t;
        if (count == 0) {
          mean = 0;
          m2 = 0;
          sum = 0;
          comp = 0;
        } else {
          double d = u - mean;
          mean -= d / count;
          m2 -= d * (u - mean);
        }
      }
      if (i < lo) continue;
      if (count < minP) {
        out[i] = Double.NaN;
        continue;
      }
      double total = sum + comp;
      out[i] = switch (k) {
        case SUM -> total;
        case MEAN -> total / count;
        case VAR -> count < 2 ? Double.NaN : Math.max(0, m2) / (count - 1);
        default -> count < 2 ? Double.NaN : Math.sqrt(Math.max(0, m2) / (count - 1));
      };
    }
  }

  private static void extreme(double[] x, int w, int minP, boolean max, int lo, int hi, double[] out) {
    int[] dq = new int[Math.min(w, hi) + 1];  // positions, values monotonic; a ring
    int head = 0, size = 0, count = 0;
    for (int i = Math.max(0, lo - w + 1), first = i; i < hi; i++) {
      int old = i - w;
      if (old >= first) {
        if (!Double.isNaN(x[old])) count--;
        if (size > 0 && dq[head] == old) {
          head = (head + 1) % dq.length;
          size--;
        }
      }
      double v = x[i];
      if (!Double.isNaN(v)) {
        count++;
        while (size > 0) {
          double last = x[dq[(head + size - 1) % dq.length]];
          if (max ? last <= v : last >= v) size--;
          else break;
        }
        dq[(head + size) % dq.length] = i;
        size++;
      }
      if (i < lo) continue;
      out[i] = count < minP || size == 0 ? Double.NaN : x[dq[head]];
    }
  }

  private static void ordered(double[] x, int w, int minP, boolean mad, double q, int lo, int hi, double[] out) {
    double[] s = new double[Math.min(w, hi)];
    int count = 0;
    for (int i = Math.max(0, lo - w + 1), first = i; i < hi; i++) {
      int old = i - w;
      if (old >= first && !Double.isNaN(x[old])) {  // remove one value equal to x[old]
        int at = Arrays.binarySearch(s, 0, count, x[old]);
        System.arraycopy(s, at + 1, s, at, count - at - 1);
        count--;
      }
      double v = x[i];
      if (!Double.isNaN(v)) {
        int at = Arrays.binarySearch(s, 0, count, v);
        if (at < 0) at = -at - 1;
        System.arraycopy(s, at, s, at + 1, count - at);
        s[at] = v;
        count++;
      }
      if (i < lo) continue;
      if (count < minP) {
        out[i] = Double.NaN;
        continue;
      }
      double[] window = count == s.length ? s : Arrays.copyOf(s, count);
      out[i] = mad ? madSorted(window) : StatsMath.quantileSorted(window, q);
    }
  }

  /**
   * The median absolute deviation of sorted values: the median of |v - median|. The deviations are
   * sorted as they are walked outward from the median, so the middle ones are found in O(n).
   */
  static double madSorted(double[] s) {
    int n = s.length;
    if (n == 0) return Double.NaN;
    double med = StatsMath.quantileSorted(s, 0.5);
    int want = (n - 1) / 2;  // the deviations' middle positions: want and, n even, want + 1
    int l = (n - 1) / 2, r = l + 1;  // the next value below (l) and above (r) to take
    if (n % 2 == 0) {
      l = n / 2 - 1;
      r = n / 2;
    }
    double a = Double.NaN, b = Double.NaN;
    for (int taken = 0; taken <= want + 1 && taken < n; taken++) {
      double dl = l >= 0 ? med - s[l] : Double.POSITIVE_INFINITY;
      double dr = r < n ? s[r] - med : Double.POSITIVE_INFINITY;
      double d;
      if (dl <= dr) {
        d = dl;
        l--;
      } else {
        d = dr;
        r++;
      }
      if (taken == want) a = d;
      if (taken == want + 1) b = d;
    }
    return n % 2 == 1 ? a : (a + b) / 2;
  }
}
