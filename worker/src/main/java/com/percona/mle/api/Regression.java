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
 * A least-squares line y = intercept + slope x ({@link Stats#linregress}), as
 * scipy.stats.linregress: r, r², the two-sided p-value of slope = 0 (Student's t, n - 2 degrees of
 * freedom), the standard errors of the slope and the intercept; n: the pairs used (both values
 * present). The sums SQL's REGR_* functions give (Oracle, PostgreSQL): {@link #n} (REGR_COUNT),
 * {@link #meanX} and {@link #meanY} (REGR_AVGX, REGR_AVGY), {@link #sxx}, {@link #syy} and {@link
 * #sxy} (REGR_SXX, REGR_SYY, REGR_SXY); {@link #slope}, {@link #intercept} and {@link #r2} are
 * REGR_SLOPE, REGR_INTERCEPT and REGR_R2. JSON with {@code String(r)} / {@code str(r)}.
 */
public final class Regression {
  private final double slope, intercept, r, p, stderr, interceptStderr;
  private final long n;
  private final double sxx, syy, sxy, meanX, meanY;

  Regression(double slope, double intercept, double r, double p, double stderr, double interceptStderr, long n,
      double[] comoments) {
    this.sxx = comoments[0];
    this.syy = comoments[1];
    this.sxy = comoments[2];
    this.meanX = comoments[3];
    this.meanY = comoments[4];
    this.slope = slope;
    this.intercept = intercept;
    this.r = r;
    this.p = p;
    this.stderr = stderr;
    this.interceptStderr = interceptStderr;
    this.n = n;
  }

  static Regression of(double[] x, double[] y) {
    double[][] c = StatsMath.complete(x, y);
    double[] a = c[0], b = c[1];
    int n = a.length;
    if (n < 2) throw new IllegalArgumentException("a regression needs at least 2 pairs of values, not " + n);
    double[] m = StatsMath.comoments(a, b);
    double ssxm = m[0] / n, ssym = m[1] / n, ssxym = m[2] / n, xmean = m[3], ymean = m[4];
    if (ssxm == 0) throw new IllegalArgumentException("all x values are equal: the slope is undefined");
    double r = ssym == 0 ? 0 : StatsMath.r(ssxm, ssym, ssxym);
    double slope = ssxym / ssxm, intercept = ymean - slope * xmean;
    double p, se, ise;
    if (n == 2) {
      p = b[0] == b[1] ? 1 : 0;
      se = 0;
      ise = 0;
    } else {
      int df = n - 2;
      double tiny = 1e-20, t = r * Math.sqrt(df / ((1 - r + tiny) * (1 + r + tiny)));
      p = 2 * StatsMath.tSf(Math.abs(t), df);
      se = Math.sqrt((1 - r * r) * ssym / ssxm / df);
      ise = se * Math.sqrt(ssxm + xmean * xmean);
    }
    return new Regression(slope, intercept, r, p, se, ise, n, m);
  }

  /** The slope (REGR_SLOPE). */
  public double slope() { return slope; }

  /** The intercept (REGR_INTERCEPT). */
  public double intercept() { return intercept; }

  /** Pearson's correlation of x and y. */
  public double r() { return r; }

  /** The share of y's variance the line explains. */
  public double r2() { return r * r; }

  /** The two-sided p-value of "the slope is 0". */
  public double pValue() { return p; }

  /** The slope's standard error. */
  public double stderr() { return stderr; }

  /** The intercept's standard error. */
  public double interceptStderr() { return interceptStderr; }

  /** The pairs used (REGR_COUNT). */
  public long n() { return n; }

  /** The mean of x over the pairs used (REGR_AVGX). */
  public double meanX() { return meanX; }

  /** The mean of y over the pairs used (REGR_AVGY). */
  public double meanY() { return meanY; }

  /** The sum of (x - meanX)² (REGR_SXX). */
  public double sxx() { return sxx; }

  /** The sum of (y - meanY)² (REGR_SYY). */
  public double syy() { return syy; }

  /** The sum of (x - meanX)(y - meanY) (REGR_SXY). */
  public double sxy() { return sxy; }

  /** intercept + slope x. */
  public double predict(double x) { return intercept + slope * x; }

  /** intercept + slope x for each x of a column (double[], numpy array, list, ...). */
  public double[] predict(Object column) {
    double[] x = GuestColumns.doubles(column, "x");
    double[] y = new double[x.length];
    for (int i = 0; i < y.length; i++) y[i] = predict(x[i]);
    return y;
  }

  /** JSON: {"slope": ..., "intercept": ..., "r": ..., "r2": ..., "p": ..., "stderr": ..., ...}. */
  @Override
  public String toString() {
    return "{\"slope\": " + Stats.json(slope) + ", \"intercept\": " + Stats.json(intercept) + ", \"r\": "
        + Stats.json(r) + ", \"r2\": " + Stats.json(r * r) + ", \"p\": " + Stats.json(p) + ", \"stderr\": "
        + Stats.json(stderr) + ", \"interceptStderr\": " + Stats.json(interceptStderr) + ", \"n\": " + n + "}";
  }
}
