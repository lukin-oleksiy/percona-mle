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
 * Statistics MySQL has no SQL for (exp notes 54, 75): percentiles, median, mode, median absolute
 * deviation, a column's summary, histograms, correlation (Pearson, Spearman) and covariance,
 * simple and multiple linear regression with p-values, t-tests, principal components, z-scores,
 * outliers, ranks, rolling windows (median, quantiles), time series; on columns passed in, or on
 * a {@link Frame} loaded from SQL (columns kept in Java, statistics per group and per time bucket). One Java implementation, the same from Java,
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

  /**
   * The times from start to stop (inclusive) every step (PostgreSQL's generate_series): step "<n>
   * <unit>" with MySQL's INTERVAL units, as '1 DAY' or '15 MINUTE'; MONTH, QUARTER and YEAR are
   * calendar steps. As MySQL writes them: dates when start is a date and the step whole days or
   * months, else date and time. For a calendar to join with: the days with no rows are the ones
   * missing from a GROUP BY.
   */
  public static String[] timeSeries(String start, String stop, String step) {
    return FrameTime.series(start, stop, step);
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

  /** The median (quantile 0.5); NaN for no values. */
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

  /**
   * The median absolute deviation: the median of |x - median(x)| (scipy.stats.median_abs_deviation
   * with scale 1, DuckDB's mad); a spread that outliers barely move. Multiply by 1.4826 for the
   * standard deviation of normal data.
   */
  public static double mad(double[] x) {
    return StatsWindows.madSorted(StatsMath.sorted(check(x)));
  }

  public static double mad(Object x) {
    return mad(GuestColumns.doubles(x, "the column"));
  }

  /**
   * A statistic over a trailing window of rows, as pandas' {@code rolling(window).<stat>()}: value i
   * covers rows i - window + 1 to i; missing values are left out; a window with fewer than
   * window values gives NaN (so do the first window - 1 rows). stat: mean, sum, var, std (n - 1),
   * min, max, median, quantile:q (as quantile:0.9), mad. A rolling mean, sum, std, min or max SQL
   * can do too (window functions); the median, quantiles and mad it cannot.
   */
  public static double[] rolling(double[] x, int window, String stat) {
    return StatsWindows.rolling(check(x), window, stat, window);
  }

  /** {@link #rolling(double[], int, String)} with a window needing only minPeriods values (1 to window). */
  public static double[] rolling(double[] x, int window, String stat, int minPeriods) {
    return StatsWindows.rolling(check(x), window, stat, minPeriods);
  }

  public static double[] rolling(Object x, int window, String stat) {
    return rolling(GuestColumns.doubles(x, "the column"), window, stat);
  }

  public static double[] rolling(Object x, int window, String stat, int minPeriods) {
    return rolling(GuestColumns.doubles(x, "the column"), window, stat, minPeriods);
  }

  // ---------------------------------------------------------------- t-tests

  /** One-sample t-test of "the mean is mu" (scipy.stats.ttest_1samp): see {@link TTest}. */
  public static TTest ttest1(double[] x, double mu) {
    return TTest.oneSample(check(x), mu);
  }

  public static TTest ttest1(Object x, double mu) {
    return ttest1(GuestColumns.doubles(x, "the column"), mu);
  }

  /**
   * Paired t-test of "the mean difference a - b is 0" (scipy.stats.ttest_rel; Oracle's
   * STATS_T_TEST_PAIRED): pairs with a missing value are left out.
   */
  public static TTest ttestPaired(double[] a, double[] b) {
    return TTest.paired(check(a), check(b));
  }

  public static TTest ttestPaired(Object a, Object b) {
    return ttestPaired(GuestColumns.doubles(a, "a"), GuestColumns.doubles(b, "b"));
  }

  /**
   * Student's t-test of two independent samples, equal variances assumed (scipy.stats.ttest_ind's
   * default; Oracle's STATS_T_TEST_INDEP). {@link #welch} does not assume them.
   */
  public static TTest ttest(double[] a, double[] b) {
    return TTest.independent(check(a), check(b), true);
  }

  public static TTest ttest(Object a, Object b) {
    return ttest(GuestColumns.doubles(a, "a"), GuestColumns.doubles(b, "b"));
  }

  /** Welch's t-test (scipy.stats.ttest_ind with equal_var=False; Oracle's STATS_T_TEST_INDEPU). */
  public static TTest welch(double[] a, double[] b) {
    return TTest.independent(check(a), check(b), false);
  }

  public static TTest welch(Object a, Object b) {
    return welch(GuestColumns.doubles(a, "a"), GuestColumns.doubles(b, "b"));
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

  // ------------------------------------------------------------------ models

  /**
   * Multiple linear regression by ordinary least squares, y on the x columns with a constant
   * (statsmodels' OLS): see {@link LinearModel}. The x columns are named x1, x2, ...; {@link
   * Frame#ols} names them by their columns.
   */
  public static LinearModel ols(double[] y, double[][] x) {
    if (x == null) throw new IllegalArgumentException("the x columns must not be null");
    String[] names = new String[x.length];
    for (int j = 0; j < names.length; j++) names[j] = "x" + (j + 1);
    return LinearModel.fit(check(y), x, names);
  }

  /** {@link #ols(double[], double[][])} for JavaScript or Python columns (a list of columns, or a 2-D numpy array of rows). */
  public static LinearModel ols(Object y, Object x) {
    return ols(GuestColumns.doubles(y, "y"), GuestColumns.columns(x, "the x columns"));
  }

  /**
   * Principal component analysis of columns (scikit-learn's PCA; centred, not scaled: scale the
   * columns first with {@link #zscore} when their units differ): see {@link Pca}. Rows with a
   * missing value are left out.
   */
  public static Pca pca(double[][] columns, int k) {
    if (columns == null) throw new IllegalArgumentException("the columns must not be null");
    return Pca.ofColumns(columns, k, null);
  }

  public static Pca pca(Object columns, int k) {
    return pca(GuestColumns.columns(columns, "the columns"), k);
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
