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
 * A t-test ({@link Stats#ttest1}, {@link Stats#ttestPaired}, {@link Stats#ttest}, {@link
 * Stats#welch}), as scipy.stats' ttest_1samp, ttest_rel and ttest_ind: the statistic t, its degrees
 * of freedom (not a whole number for Welch's test), and the p-values of the three alternatives:
 * two-sided ({@link #p}), "less" and "greater". JSON with {@code String(t)} / {@code str(t)}.
 */
public final class TTest {
  private final double t, df, difference;
  private final long n1, n2;

  TTest(double t, double df, double difference, long n1, long n2) {
    this.t = t;
    this.df = df;
    this.difference = difference;
    this.n1 = n1;
    this.n2 = n2;
  }

  /** The t statistic. */
  public double statistic() { return t; }

  /** The degrees of freedom. */
  public double df() { return df; }

  /** The two-sided p-value: the means (or the mean and mu) differ. */
  public double p() {
    if (Double.isNaN(t)) return Double.NaN;
    return Math.min(1, 2 * StatsMath.tSf(Math.abs(t), df));
  }

  /** The p-value of "the first mean is less" (the alternative 'less'). */
  public double pLess() {
    return Double.isNaN(t) ? Double.NaN : StatsMath.tSf(-t, df);
  }

  /** The p-value of "the first mean is greater" (the alternative 'greater'). */
  public double pGreater() {
    return Double.isNaN(t) ? Double.NaN : StatsMath.tSf(t, df);
  }

  /** The difference the test is about: mean - mu, the mean of the differences, or mean1 - mean2. */
  public double difference() { return difference; }

  /** The values used of the first (or only) column. */
  public long n1() { return n1; }

  /** The values used of the second column (paired and one-sample tests: the same as n1). */
  public long n2() { return n2; }

  // ---------------------------------------------------------------- tests

  static TTest oneSample(double[] x, double mu) {
    if (Double.isNaN(mu) || Double.isInfinite(mu)) throw new IllegalArgumentException("mu must be a finite number, not " + mu);
    double[] s = StatsMath.present(x);
    int n = s.length;
    double m = StatsMath.mean(s);
    double v = n < 2 ? Double.NaN : StatsMath.centralSums(s, m)[0] / (n - 1);
    return new TTest((m - mu) / Math.sqrt(v / n), n - 1, m - mu, n, n);
  }

  static TTest paired(double[] a, double[] b) {
    double[][] p = StatsMath.complete(a, b);
    double[] d = new double[p[0].length];
    for (int i = 0; i < d.length; i++) d[i] = p[0][i] - p[1][i];
    TTest r = oneSample(d, 0);
    return new TTest(r.t, r.df, r.difference, d.length, d.length);
  }

  static TTest independent(double[] a, double[] b, boolean equalVariances) {
    double[] x = StatsMath.present(a), y = StatsMath.present(b);
    int n1 = x.length, n2 = y.length;
    double m1 = StatsMath.mean(x), m2 = StatsMath.mean(y);
    double v1 = n1 < 2 ? Double.NaN : StatsMath.centralSums(x, m1)[0] / (n1 - 1);
    double v2 = n2 < 2 ? Double.NaN : StatsMath.centralSums(y, m2)[0] / (n2 - 1);
    double t, df;
    if (equalVariances) {
      df = n1 + n2 - 2;
      double sp = ((n1 - 1) * v1 + (n2 - 1) * v2) / df;
      t = (m1 - m2) / Math.sqrt(sp * (1.0 / n1 + 1.0 / n2));
    } else {
      double a1 = v1 / n1, a2 = v2 / n2, se = a1 + a2;
      df = se * se / (a1 * a1 / (n1 - 1) + a2 * a2 / (n2 - 1));
      t = (m1 - m2) / Math.sqrt(se);
    }
    return new TTest(t, df, m1 - m2, n1, n2);
  }

  /** JSON: {"statistic": ..., "df": ..., "p": ..., "pLess": ..., "pGreater": ..., "difference": ..., ...}. */
  @Override
  public String toString() {
    return "{\"statistic\": " + Stats.json(t) + ", \"df\": " + Stats.json(df) + ", \"p\": " + Stats.json(p())
        + ", \"pLess\": " + Stats.json(pLess()) + ", \"pGreater\": " + Stats.json(pGreater()) + ", \"difference\": "
        + Stats.json(difference) + ", \"n1\": " + n1 + ", \"n2\": " + n2 + "}";
  }
}
