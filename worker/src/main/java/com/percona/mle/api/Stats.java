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
 * Statistics MySQL has no SQL for (exp note 54): percentiles, median, mode, a column's summary,
 * histograms, correlation (Pearson, Spearman) and covariance, simple linear regression with its
 * p-value, z-scores, outliers, ranks; on columns passed in, or on a {@link Frame} loaded from SQL
 * (columns kept in Java, statistics per group). One Java implementation, the same from Java,
 * JavaScript ({@code Java.type('com.percona.mle.api.Stats')}) and Python ({@code
 * java.type(...)}).
 *
 * <p>Columns are {@code double[]} in Java; JavaScript and Python pass a numpy array, a
 * Float64Array, an array or a list (the {@code Object} overloads). A missing value (SQL NULL,
 * None, null, NaN) is left out, as pandas does; pairs leave out a pair with a missing value on
 * either side. Formulas follow numpy, pandas and scipy, whose results the tests compare with.
 */
public final class Stats {
  private Stats() {}

  // ---------------------------------------------------------------- from SQL

  /**
   * Runs a query and keeps its columns in Java: numbers (and booleans) as numbers, anything else as
   * text; NULL is missing. Runs SQL, so only in stored procedures; the rows count towards
   * mle.sql_result_max.
   */
  public static Frame load(String sql, Object... params) {
    return Frame.load(sql, params);
  }

  // -------------------------------------------------------------- one column

  /** count, missing, mean, std, min, quartiles, max, skewness, kurtosis: see {@link Summary}. */
  public static Summary describe(double[] x) {
    return Summary.of(check(x));
  }

  /** {@link #describe(double[])} for a JavaScript or Python column. */
  public static Summary describe(Object x) {
    return describe(GuestColumns.doubles(x, "the column"));
  }

  /** Quantile q (0 to 1) by linear interpolation (numpy's and pandas' default); NaN for no values. */
  public static double quantile(double[] x, double q) {
    return StatsMath.quantileSorted(StatsMath.sorted(check(x)), q);
  }

  /** {@link #quantile(double[], double)} for a JavaScript or Python column. */
  public static double quantile(Object x, double q) {
    return quantile(GuestColumns.doubles(x, "the column"), q);
  }

  /** Several quantiles with one sort. */
  public static double[] quantiles(double[] x, double[] qs) {
    double[] s = StatsMath.sorted(check(x));
    if (qs == null) throw new IllegalArgumentException("the quantiles must not be null");
    double[] r = new double[qs.length];
    for (int i = 0; i < r.length; i++) r[i] = StatsMath.quantileSorted(s, qs[i]);
    return r;
  }

  /** {@link #quantiles(double[], double[])} for JavaScript or Python arrays. */
  public static double[] quantiles(Object x, Object qs) {
    return quantiles(GuestColumns.doubles(x, "the column"), GuestColumns.doubles(qs, "the quantiles"));
  }

  public static double median(double[] x) {
    return quantile(x, 0.5);
  }

  public static double median(Object x) {
    return median(GuestColumns.doubles(x, "the column"));
  }

  /** The most frequent value; a tie goes to the smallest (scipy.stats.mode). */
  public static double mode(double[] x) {
    return StatsMath.modeSorted(StatsMath.sorted(check(x)));
  }

  public static double mode(Object x) {
    return mode(GuestColumns.doubles(x, "the column"));
  }

  /** The interquartile range q3 - q1. */
  public static double iqr(double[] x) {
    double[] s = StatsMath.sorted(check(x));
    return StatsMath.quantileSorted(s, 0.75) - StatsMath.quantileSorted(s, 0.25);
  }

  public static double iqr(Object x) {
    return iqr(GuestColumns.doubles(x, "the column"));
  }

  /** Equal-width bins between the smallest and the largest value: see {@link Histogram}. */
  public static Histogram histogram(double[] x, int bins) {
    return Histogram.of(check(x), bins);
  }

  public static Histogram histogram(Object x, int bins) {
    return histogram(GuestColumns.doubles(x, "the column"), bins);
  }

  /** Bins between the given edges (increasing). */
  public static Histogram histogramEdges(double[] x, double[] edges) {
    return Histogram.of(check(x), edges);
  }

  public static Histogram histogramEdges(Object x, Object edges) {
    return histogramEdges(GuestColumns.doubles(x, "the column"), GuestColumns.doubles(edges, "the edges"));
  }

  /** (x - mean) / std with the population std (n, as scipy.stats.zscore); missing stays missing. */
  public static double[] zscore(double[] x) {
    double[] s = StatsMath.sorted(check(x));
    double m = StatsMath.mean(s);
    double sd = s.length == 0 ? Double.NaN : Math.sqrt(StatsMath.centralSums(s, m)[0] / s.length);
    double[] z = new double[x.length];
    for (int i = 0; i < z.length; i++) z[i] = (x[i] - m) / sd;  // NaN stays NaN; sd 0 gives NaN or inf
    if (sd == 0) java.util.Arrays.fill(z, Double.NaN);
    return z;
  }

  public static double[] zscore(Object x) {
    return zscore(GuestColumns.doubles(x, "the column"));
  }

  /**
   * The positions of outliers: 'iqr' (Tukey's fences): below q1 - k·IQR or above q3 + k·IQR (k 1.5
   * is usual, 3 for far out); 'zscore': |z| above k (3 is usual).
   */
  public static int[] outliers(double[] x, String method, double k) {
    check(x);
    if (!(k >= 0)) throw new IllegalArgumentException("k must be at least 0, not " + k);
    String m = method == null ? "" : method.trim().toLowerCase(java.util.Locale.ROOT);
    double[] score;
    double lo, hi;
    switch (m) {
      case "iqr" -> {
        double[] s = StatsMath.sorted(x);
        double q1 = StatsMath.quantileSorted(s, 0.25), q3 = StatsMath.quantileSorted(s, 0.75);
        lo = q1 - k * (q3 - q1);
        hi = q3 + k * (q3 - q1);
        score = x;
      }
      case "zscore" -> {
        score = zscore(x);
        lo = -k;
        hi = k;
      }
      default -> throw new IllegalArgumentException("unknown method '" + method + "' (iqr or zscore)");
    }
    int n = 0;
    for (double v : score)
      if (v < lo || v > hi) n++;
    int[] r = new int[n];
    for (int i = 0, o = 0; i < score.length; i++)
      if (score[i] < lo || score[i] > hi) r[o++] = i;
    return r;
  }

  public static int[] outliers(Object x, String method, double k) {
    return outliers(GuestColumns.doubles(x, "the column"), method, k);
  }

  /** Ranks from 1; ties get their average rank (scipy.stats.rankdata); missing stays missing. */
  public static double[] rank(double[] x) {
    return StatsMath.rank(check(x));
  }

  public static double[] rank(Object x) {
    return rank(GuestColumns.doubles(x, "the column"));
  }

  // ------------------------------------------------------------ two columns

  /** 'pearson' or 'spearman' (Pearson's of the ranks); NaN when a column has no spread. */
  public static double correlation(double[] x, double[] y, String method) {
    return StatsMath.correlation(check(x), check(y), method);
  }

  public static double correlation(Object x, Object y, String method) {
    return correlation(GuestColumns.doubles(x, "x"), GuestColumns.doubles(y, "y"), method);
  }

  /** The sample covariance (n - 1). */
  public static double covariance(double[] x, double[] y) {
    double[][] p = StatsMath.complete(check(x), check(y));
    if (p[0].length < 2) return Double.NaN;
    return StatsMath.comoments(p[0], p[1])[2] / (p[0].length - 1);
  }

  public static double covariance(Object x, Object y) {
    return covariance(GuestColumns.doubles(x, "x"), GuestColumns.doubles(y, "y"));
  }

  /** Every pair of columns' correlation (each pair with its own complete values, as pandas' corr()). */
  public static double[][] correlationMatrix(double[][] columns, String method) {
    if (columns == null) throw new IllegalArgumentException("the columns must not be null");
    int c = columns.length;
    double[][] m = new double[c][c];
    for (int i = 0; i < c; i++)
      for (int j = i; j < c; j++) m[i][j] = m[j][i] = correlation(columns[i], columns[j], method);
    return m;
  }

  public static double[][] correlationMatrix(Object columns, String method) {
    return correlationMatrix(GuestColumns.columns(columns, "the columns"), method);
  }

  /** The least-squares line of y on x: see {@link Regression}. */
  public static Regression linregress(double[] x, double[] y) {
    return Regression.of(check(x), check(y));
  }

  public static Regression linregress(Object x, Object y) {
    return linregress(GuestColumns.doubles(x, "x"), GuestColumns.doubles(y, "y"));
  }

  // ---------------------------------------------------------------- helpers

  private static double[] check(double[] x) {
    if (x == null) throw new IllegalArgumentException("the column must not be null");
    return x;
  }

  /** A number for JSON: NaN and infinities as null. */
  static String json(double v) {
    if (!Double.isFinite(v)) return "null";
    if (v == Math.rint(v) && Math.abs(v) < 1e15) return Long.toString((long) v);
    return Double.toString(v);
  }
}
