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
 * A column's summary ({@link Stats#describe}), as pandas' describe() plus what is missing,
 * skewness and kurtosis: count (values that are not missing), missing (NULL / NaN), mean, std
 * (sample, n - 1), min, q1, median, q3, max (quartiles by linear interpolation), skewness and
 * kurtosis (excess; bias-corrected, as pandas' skew() and kurt()). NaN where a value needs more
 * values than there are. JSON with {@code String(s)} / {@code str(s)}.
 */
public final class Summary {
  private final long count, missing;
  private final double mean, std, min, q1, median, q3, max, skewness, kurtosis;

  Summary(long count, long missing, double mean, double std, double min, double q1, double median, double q3,
      double max, double skewness, double kurtosis) {
    this.count = count;
    this.missing = missing;
    this.mean = mean;
    this.std = std;
    this.min = min;
    this.q1 = q1;
    this.median = median;
    this.q3 = q3;
    this.max = max;
    this.skewness = skewness;
    this.kurtosis = kurtosis;
  }

  static Summary of(double[] x) {
    double[] s = StatsMath.sorted(x);
    int n = s.length;
    double m = StatsMath.mean(s);
    double[] c = StatsMath.centralSums(s, m);
    double std = n > 1 ? Math.sqrt(c[0] / (n - 1)) : Double.NaN;
    double m2 = c[0] / n, m3 = c[1] / n, m4 = c[2] / n;
    double skew = n < 3 ? Double.NaN : m2 == 0 ? 0 : Math.sqrt((double) n * (n - 1)) / (n - 2) * m3 / Math.pow(m2, 1.5);
    double kurt = n < 4 ? Double.NaN : m2 == 0 ? 0
        : (n - 1.0) / ((n - 2.0) * (n - 3.0)) * ((n + 1.0) * m4 / (m2 * m2) - 3.0 * (n - 1));
    return new Summary(n, x.length - n, m, std, n > 0 ? s[0] : Double.NaN, StatsMath.quantileSorted(s, 0.25),
        StatsMath.quantileSorted(s, 0.5), StatsMath.quantileSorted(s, 0.75), n > 0 ? s[n - 1] : Double.NaN, skew,
        kurt);
  }

  public long count() { return count; }

  public long missing() { return missing; }

  public double mean() { return mean; }

  public double std() { return std; }

  public double min() { return min; }

  public double q1() { return q1; }

  public double median() { return median; }

  public double q3() { return q3; }

  public double max() { return max; }

  /** q3 - q1. */
  public double iqr() { return q3 - q1; }

  public double skewness() { return skewness; }

  public double kurtosis() { return kurtosis; }

  /** JSON: {"count": 9, "missing": 1, "mean": ..., ...}; NaN as null. */
  @Override
  public String toString() {
    return "{\"count\": " + count + ", \"missing\": " + missing + ", \"mean\": " + Stats.json(mean) + ", \"std\": "
        + Stats.json(std) + ", \"min\": " + Stats.json(min) + ", \"q1\": " + Stats.json(q1) + ", \"median\": "
        + Stats.json(median) + ", \"q3\": " + Stats.json(q3) + ", \"max\": " + Stats.json(max) + ", \"skewness\": "
        + Stats.json(skewness) + ", \"kurtosis\": " + Stats.json(kurtosis) + "}";
  }
}
