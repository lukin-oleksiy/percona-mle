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
 * A linear model y = b0 + b1 x1 + ... + bk xk fitted by ordinary least squares ({@link Stats#ols},
 * {@link Frame#ols}), as statsmodels' OLS with a constant: coefficients (the constant first, named
 * "const"), their standard errors, t statistics and two-sided p-values, R², adjusted R², the F
 * statistic of "all slopes are 0" and its p-value, the log-likelihood, AIC and BIC. A row with a
 * missing value in y or in any x is left out (statsmodels' missing='drop'). JSON with {@code
 * String(m)} / {@code str(m)}.
 *
 * <p>Fitted by a blocked Householder QR of [1 x1 ... xk y] (no normal equations, so no squared
 * condition number); blocks of rows on the vector pool, combined in order.
 */
public final class LinearModel {
  private final String[] names;
  private final double[] coef, se;
  private final long n;
  private final int p;
  private final double rss, tss;

  private LinearModel(String[] names, double[] coef, double[] se, long n, double rss, double tss) {
    this.names = names;
    this.coef = coef;
    this.se = se;
    this.n = n;
    this.p = coef.length;
    this.rss = rss;
    this.tss = tss;
  }

  static LinearModel fit(double[] y, double[][] x, String[] xNames) {
    if (y == null || x == null) throw new IllegalArgumentException("y and the x columns must not be null");
    int k = x.length;
    if (k == 0) throw new IllegalArgumentException("a model needs at least one x column");
    for (int j = 0; j < k; j++) {
      if (x[j] == null) throw new IllegalArgumentException("x column " + j + " is null");
      if (x[j].length != y.length)
        throw new IllegalArgumentException("y has " + y.length + " values, x column " + xNames[j] + " " + x[j].length);
    }
    int total = y.length, n = 0;
    int[] rows = new int[total];
    for (int i = 0; i < total; i++) {
      boolean ok = !Double.isNaN(y[i]);
      for (int j = 0; j < k && ok; j++) ok = !Double.isNaN(x[j][i]);
      if (ok) rows[n++] = i;
    }
    int p = k + 1;
    if (n <= p)
      throw new IllegalArgumentException("a model with " + p + " coefficients needs more than " + p
          + " complete rows, not " + n);
    double[] ones = new double[total];
    java.util.Arrays.fill(ones, 1);
    double[][] cols = new double[p + 1][];
    cols[0] = ones;
    System.arraycopy(x, 0, cols, 1, k);
    cols[p] = y;
    double[][] r = StatsLinear.r(cols, rows, n);
    String[] names = new String[p];
    names[0] = "const";
    System.arraycopy(xNames, 0, names, 1, k);
    for (int i = 0; i < p; i++) {  // |R_ii|: the part of column i the columns before it do not explain
      double norm = 0;
      for (int t = 0; t <= i; t++) norm += r[t][i] * r[t][i];
      if (!(Math.abs(r[i][i]) > 1e-10 * Math.sqrt(norm)))
        throw new IllegalArgumentException("column " + names[i] + (i == 0 ? " is all zero"
            : " is constant or a linear combination of the columns before it") + ": leave it out");
    }
    double[] coef = new double[p];
    for (int i = p - 1; i >= 0; i--) {  // R11 b = R[0..p-1][p]
      double s = r[i][p];
      for (int t = i + 1; t < p; t++) s -= r[i][t] * coef[t];
      coef[i] = s / r[i][i];
    }
    double rss = r[p][p] * r[p][p];
    double[][] inv = StatsLinear.inverseUpper(r, p);
    double scale = rss / (n - p);
    double[] se = new double[p];
    for (int i = 0; i < p; i++) {
      double s = 0;
      for (int t = i; t < p; t++) s += inv[i][t] * inv[i][t];
      se[i] = Math.sqrt(scale * s);
    }
    double[] yc = new double[n];
    for (int i = 0; i < n; i++) yc[i] = y[rows[i]];
    double tss = StatsMath.centralSums(yc, StatsMath.mean(yc))[0];
    return new LinearModel(names, coef, se, n, rss, tss);
  }

  /** "const", then the x columns' names. */
  public String[] names() { return names.clone(); }

  /** The coefficients: the constant, then one per x column. */
  public double[] coefficients() { return coef.clone(); }

  /** The coefficient of a column by name ("const": the constant). */
  public double coefficient(String name) {
    for (int i = 0; i < p; i++)
      if (names[i].equals(name)) return coef[i];
    for (int i = 0; i < p; i++)
      if (names[i].equalsIgnoreCase(name)) return coef[i];
    throw new IllegalArgumentException("no coefficient '" + name + "' (" + String.join(", ", names) + ")");
  }

  /** The coefficients' standard errors. */
  public double[] stderr() { return se.clone(); }

  /** The coefficients' t statistics. */
  public double[] t() {
    double[] t = new double[p];
    for (int i = 0; i < p; i++) t[i] = coef[i] / se[i];
    return t;
  }

  /** The two-sided p-values of "the coefficient is 0" (Student's t, n - k - 1 degrees of freedom). */
  public double[] p() {
    double[] t = t(), pv = new double[p];
    for (int i = 0; i < p; i++) pv[i] = Double.isNaN(t[i]) ? Double.NaN : Math.min(1, 2 * StatsMath.tSf(Math.abs(t[i]), dfResidual()));
    return pv;
  }

  /** The rows used. */
  public long n() { return n; }

  /** k: the x columns. */
  public int dfModel() { return p - 1; }

  /** n - k - 1. */
  public long dfResidual() { return n - p; }

  /** The share of y's variance the model explains. */
  public double r2() { return tss == 0 ? Double.NaN : 1 - rss / tss; }

  /** R² adjusted for the number of columns. */
  public double adjustedR2() { return 1 - (1 - r2()) * (n - 1) / (n - p); }

  /** The sum of squared residuals. */
  public double ssr() { return rss; }

  /** The residuals' standard deviation, sqrt(ssr / (n - k - 1)). */
  public double residualStd() { return Math.sqrt(rss / (n - p)); }

  /** The F statistic of "every coefficient but the constant is 0". */
  public double f() { return ((tss - rss) / (p - 1)) / (rss / (n - p)); }

  /** The p-value of the F statistic. */
  public double fP() {
    double f = f(), d1 = p - 1, d2 = n - p;
    if (Double.isNaN(f)) return Double.NaN;
    if (Double.isInfinite(f)) return 0;
    return StatsMath.betai(d2 / 2, d1 / 2, d2 / (d2 + d1 * f));
  }

  /** The log-likelihood (normal errors). */
  public double logLikelihood() { return -n / 2.0 * (Math.log(2 * Math.PI) + Math.log(rss / n) + 1); }

  /** Akaike's information criterion: -2 log-likelihood + 2 (k + 1). */
  public double aic() { return -2 * logLikelihood() + 2 * p; }

  /** Bayes' information criterion: -2 log-likelihood + (k + 1) ln n. */
  public double bic() { return -2 * logLikelihood() + p * Math.log(n); }

  /** The model's values for x columns in the model's order (k columns of one length); a missing x: NaN. */
  public double[] predict(double[][] x) {
    if (x == null || x.length != p - 1)
      throw new IllegalArgumentException("the model has " + (p - 1) + " x columns, not " + (x == null ? 0 : x.length));
    int len = x.length == 0 ? 0 : x[0].length;
    double[] y = new double[len];
    for (int i = 0; i < len; i++) {
      double s = coef[0];
      for (int j = 1; j < p; j++) {
        if (x[j - 1].length != len) throw new IllegalArgumentException("the x columns have different lengths");
        s += coef[j] * x[j - 1][i];
      }
      y[i] = s;
    }
    return y;
  }

  /** {@link #predict(double[][])} for JavaScript or Python columns. */
  public double[] predict(Object x) {
    return predict(GuestColumns.columns(x, "the x columns"));
  }

  /** The model's values for a Frame's rows, its x columns found by name. */
  public double[] predict(Frame f) {
    double[][] x = new double[p - 1][];
    for (int j = 1; j < p; j++) x[j - 1] = f.column(names[j]);
    return predict(x);
  }

  /** JSON: {"names": [...], "coefficients": [...], "stderr": [...], "t": [...], "p": [...], "r2": ..., ...}. */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("{\"names\": [");
    for (int i = 0; i < p; i++) Frame.quote(b.append(i > 0 ? ", " : ""), names[i]);
    b.append("], \"coefficients\": ").append(array(coef)).append(", \"stderr\": ").append(array(se))
        .append(", \"t\": ").append(array(t())).append(", \"p\": ").append(array(p()))
        .append(", \"r2\": ").append(Stats.json(r2())).append(", \"adjustedR2\": ").append(Stats.json(adjustedR2()))
        .append(", \"f\": ").append(Stats.json(f())).append(", \"fP\": ").append(Stats.json(fP()))
        .append(", \"n\": ").append(n).append(", \"aic\": ").append(Stats.json(aic())).append("}");
    return b.toString();
  }

  static String array(double[] v) {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < v.length; i++) b.append(i > 0 ? ", " : "").append(Stats.json(v[i]));
    return b.append(']').toString();
  }
}
