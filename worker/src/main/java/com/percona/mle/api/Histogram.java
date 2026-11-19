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
 * A histogram ({@link Stats#histogram}): bins+1 edges and the count of each bin, as numpy's
 * histogram: bins are [edge i, edge i+1), the last one closed; missing values and values outside
 * the edges are not counted. JSON with {@code String(h)} / {@code str(h)}.
 */
public final class Histogram {
  private final double[] edges;
  private final long[] counts;

  Histogram(double[] edges, long[] counts) {
    this.edges = edges;
    this.counts = counts;
  }

  /**
   * Equal-width bins between the smallest and the largest value. When they are equal: ±0.5 around
   * the value, or wider when the value is so large that 0.5 is below its precision. Values too
   * close together for this many distinct edges are an error.
   */
  static Histogram of(double[] x, int bins) {
    if (bins < 1) throw new IllegalArgumentException("bins must be at least 1, not " + bins);
    double lo = Double.POSITIVE_INFINITY, hi = Double.NEGATIVE_INFINITY;
    for (double v : x)
      if (!Double.isNaN(v)) {
        lo = Math.min(lo, v);
        hi = Math.max(hi, v);
      }
    if (lo > hi) {  // no values
      lo = 0;
      hi = 1;
    } else if (Double.isInfinite(lo) || Double.isInfinite(hi)) {
      throw new IllegalArgumentException("the values include an infinity: give the edges");
    } else if (lo == hi) {
      double w = Math.max(0.5, 2 * Math.ulp(lo) * bins);
      lo = Math.max(-Double.MAX_VALUE, lo - w);
      hi = Math.min(Double.MAX_VALUE, hi + w);
    }
    double[] e = new double[bins + 1];
    double width = hi - lo;
    if (Double.isFinite(width * bins)) {
      for (int i = 0; i <= bins; i++) e[i] = lo + width * i / bins;
    } else {  // hi - lo (or its multiple) overflows: weigh the two ends instead
      for (int i = 0; i <= bins; i++) {
        double t = (double) i / bins;
        e[i] = lo * (1 - t) + hi * t;
      }
    }
    e[0] = lo;
    e[bins] = hi;
    for (int i = 1; i <= bins; i++)
      if (!(e[i] > e[i - 1]))
        throw new IllegalArgumentException("the values span too little for " + bins
            + " bins (" + lo + " to " + hi + "): give the edges or fewer bins");
    return of(x, e);
  }

  /** The given edges (increasing). */
  static Histogram of(double[] x, double[] edges) {
    if (edges == null || edges.length < 2) throw new IllegalArgumentException("at least 2 edges are needed");
    for (int i = 1; i < edges.length; i++)
      if (!(edges[i] > edges[i - 1])) throw new IllegalArgumentException("the edges must increase");
    int bins = edges.length - 1;
    long[] c = new long[bins];
    double lo = edges[0], hi = edges[bins];
    for (double v : x) {
      if (!(v >= lo && v <= hi)) continue;  // NaN too
      int i = (int) ((v - lo) / (hi - lo) * bins);
      if (i >= bins) i = bins - 1;
      while (i > 0 && v < edges[i]) i--;
      while (i < bins - 1 && v >= edges[i + 1]) i++;
      c[i]++;
    }
    return new Histogram(edges.clone(), c);
  }

  /** The number of bins. */
  public int bins() { return counts.length; }

  /** The edges (bins + 1; a copy). */
  public double[] edges() { return edges.clone(); }

  /** The counts (a copy). */
  public long[] counts() { return counts.clone(); }

  /** Bin i's count. */
  public long count(int i) { return counts[i]; }

  /** JSON: {"edges": [...], "counts": [...]}. */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("{\"edges\": [");
    for (int i = 0; i < edges.length; i++) b.append(i > 0 ? ", " : "").append(Stats.json(edges[i]));
    b.append("], \"counts\": [");
    for (int i = 0; i < counts.length; i++) b.append(i > 0 ? ", " : "").append(counts[i]);
    return b.append("]}").toString();
  }
}
