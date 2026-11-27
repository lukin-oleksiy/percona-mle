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

import com.percona.mle.sql.Column;
import com.percona.mle.sql.Row;
import com.percona.mle.sql.SqlResult;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.LocalDateTime;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Columns from SQL kept in Java ({@link Stats#load}), for statistics per column and per group
 * (exp note 54): numbers as double (NULL: NaN, missing), anything else as text (NULL: null).
 * Results that are tables (per group, a correlation matrix, a selection of rows) are Frames too;
 * {@code String(t)} / {@code str(t)} gives JSON rows, {@link #rows()} rows for executeMany.
 *
 * <p>A column holds numbers when its SQL type is numeric (integers, DECIMAL, FLOAT, DOUBLE, YEAR,
 * BIT), also with no rows or only NULLs; any other column is text: strings as they are, DATE,
 * TIME, DATETIME and TIMESTAMP as MySQL prints them, a VECTOR as [1.0, 2.0].
 *
 * <p>Every number is a double, BIGINT and DECIMAL values too: an integer beyond 2^53 (about 9e15)
 * or a DECIMAL of more than 15 digits is rounded to the nearest double. Two such values that
 * differ only in their last digits (64-bit ids: UUID_SHORT(), snowflake ids) become equal, fall
 * into one group of {@link #groupBy} and come back rounded from {@link #rows()} and {@link
 * #text}. Select such a column as text ({@code CAST(id AS CHAR)}) to keep it exact: it then
 * groups by its exact value.
 *
 * <p>Time series (exp note 75): a DATE, DATETIME or TIMESTAMP column is text as MySQL prints it
 * (so {@link #text}, JSON and {@link #rows()} give MySQL's own format, which a DATETIME parameter
 * takes), and the frame knows its type: {@link #bucket} puts its times into buckets of a width
 * (then {@link #groupBy} for statistics per bucket), {@link #gapFill} adds the missing steps of a
 * series, {@link #seconds} gives its times as numbers. A time is the wall-clock value MySQL
 * returned, with no time zone (a TIMESTAMP in the session's zone).
 */
public final class Frame {
  private final String[] names;
  private final double[][] num;  // per column, null for text
  private final String[][] text;  // per column, null for numbers
  private final String[] types;  // per column: DATE, DATETIME(d), TIMESTAMP(d) for a time column, else null
  private final int size;

  Frame(String[] names, double[][] num, String[][] text, int size) {
    this(names, num, text, new String[names.length], size);
  }

  Frame(String[] names, double[][] num, String[][] text, String[] types, int size) {
    this.names = names;
    this.num = num;
    this.text = text;
    this.types = types;
    this.size = size;
  }

  static Frame load(String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    // The rows are for the frame, whatever the routine set for its own statements.
    SqlResult r = Mle.session().runSql(sql, params == null ? new Object[0] : params,
        new com.percona.mle.sql.SqlOptions().passResultToClient(false));
    String[] names = r.getColumnNames().toArray(new String[0]);
    List<Column> meta = r.getColumns();
    int c = names.length;
    List<Object[]> rows = new ArrayList<>();
    for (Row row : r) rows.add(row.toArray());
    int n = rows.size();
    double[][] num = new double[c][];
    String[][] text = new String[c][];
    String[] types = new String[c];
    for (int j = 0; j < c; j++) {
      // By the column's type, not only by its values: a text column with no rows, or with only
      // NULLs, is text. The values are checked too (a numeric type whose values are not numbers).
      Column column = j < meta.size() ? meta.get(j) : null;
      boolean numbers = column == null || numericType(column);
      if (numbers)
        for (Object[] row : rows) {
          Object v = row[j];
          if (v != null && !(v instanceof Number) && !(v instanceof Boolean)) {
            numbers = false;
            break;
          }
        }
      if (numbers) {
        double[] d = num[j] = new double[n];
        for (int i = 0; i < n; i++) {
          Object v = rows.get(i)[j];
          d[i] = v == null ? Double.NaN : v instanceof Boolean b ? (b ? 1 : 0) : ((Number) v).doubleValue();
        }
      } else {
        String[] t = text[j] = new String[n];
        for (int i = 0; i < n; i++) {
          Object v = rows.get(i)[j];
          t[i] = v == null ? null : asText(v, column == null ? 0 : column.getFractionalDigits());
        }
        types[j] = timeType(column);
      }
    }
    return new Frame(names, num, text, types, n);
  }

  /**
   * Whether a result column's MySQL type holds numbers. A type without a name (a bare NULL, a
   * string expression) may: unless it has a character set, its values decide.
   */
  private static boolean numericType(Column column) {
    String type = column.getSqlType();
    if (type == null) return true;
    return switch (type) {
      case "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT", "YEAR", "BIT", "FLOAT", "DOUBLE", "DECIMAL" -> true;
      case "" -> !"STRING".equals(column.getType());
      default -> false;
    };
  }

  /** DATE, DATETIME(d) or TIMESTAMP(d) for a time column, else null. */
  private static String timeType(Column column) {
    String type = column == null ? null : column.getSqlType();
    if (type == null) return null;
    return switch (type) {
      case "DATE" -> "DATE";
      case "DATETIME", "TIMESTAMP" -> {
        int d = Math.max(0, Math.min(6, column.getFractionalDigits()));
        yield d == 0 ? type : type + "(" + d + ")";
      }
      default -> null;
    };
  }

  /** A value of a text column as MySQL prints it (decimals: the column's fractional seconds). */
  private static String asText(Object v, int decimals) {
    if (v instanceof byte[] b) return new String(b, StandardCharsets.UTF_8);
    if (v instanceof float[] f) return Arrays.toString(f);  // VECTOR
    if (v instanceof Duration d) {  // TIME: [-]HH:MM:SS, hours up to 838
      Duration a = d.isNegative() ? d.negated() : d;
      long s = a.getSeconds();
      return (d.isNegative() ? "-" : "") + clock(s / 3600, (int) (s / 60 % 60), (int) (s % 60), a.getNano(), decimals);
    }
    if (v instanceof java.time.Instant i)  // TIMESTAMP: in the session's zone, as MySQL shows it
      v = LocalDateTime.ofInstant(i, com.percona.mle.worker.SqlBridge.sessionZone());
    if (v instanceof LocalDateTime t)  // DATETIME, TIMESTAMP: a space, not ISO 8601's T
      return t.toLocalDate() + " " + clock(t.getHour(), t.getMinute(), t.getSecond(), t.getNano(), decimals);
    return String.valueOf(v);
  }

  private static String clock(long h, int m, int s, int nano, int decimals) {
    String t = String.format(Locale.ROOT, "%02d:%02d:%02d", h, m, s);
    int digits = Math.max(0, Math.min(6, decimals));
    return digits == 0 ? t : t + "." + String.format(Locale.ROOT, "%06d", nano / 1000).substring(0, digits);
  }

  /** The number of rows. */
  public int size() {
    return size;
  }

  /** The column names. */
  public String[] columns() {
    return names.clone();
  }

  /** Whether the column holds numbers. */
  public boolean numeric(String name) {
    return num[index(name)] != null;
  }

  /** A column of numbers (a copy; missing values NaN). */
  public double[] column(String name) {
    return numbers(name).clone();
  }

  /** A column as text (numbers formatted; missing: null). */
  public String[] text(String name) {
    int j = index(name);
    if (text[j] != null) return text[j].clone();
    String[] t = new String[size];
    for (int i = 0; i < size; i++) t[i] = Double.isNaN(num[j][i]) ? null : Stats.json(num[j][i]);
    return t;
  }

  /**
   * This frame with a column of numbers added at the end, or replacing the column of that name: a
   * computed column (a rolling median, a prediction, z-scores) next to the data. values: one per row.
   */
  public Frame with(String name, double[] values) {
    if (values == null) throw new IllegalArgumentException("the values must not be null");
    return withColumn(name, values, null);
  }

  /** {@link #with(String, double[])} for a column of text (null: missing). */
  public Frame with(String name, String[] values) {
    if (values == null) throw new IllegalArgumentException("the values must not be null");
    return withColumn(name, null, values);
  }

  /** {@link #with(String, double[])} for a JavaScript or Python column of numbers. */
  public Frame with(String name, Object values) {
    return withColumn(name, GuestColumns.doubles(values, "the values"), null);
  }

  private Frame withColumn(String name, double[] nums, String[] texts) {
    if (name == null || name.isEmpty()) throw new IllegalArgumentException("the column needs a name");
    int len = nums != null ? nums.length : texts.length;
    if (len != size) throw new IllegalArgumentException("the frame has " + size + " rows, the column " + len);
    int at = -1;
    for (int j = 0; j < names.length && at < 0; j++)
      if (names[j].equalsIgnoreCase(name)) at = j;
    int c = at >= 0 ? names.length : names.length + 1;
    String[] n2 = Arrays.copyOf(names, c);
    double[][] num2 = Arrays.copyOf(num, c);
    String[][] text2 = Arrays.copyOf(text, c);
    String[] types2 = Arrays.copyOf(types, c);
    int j = at >= 0 ? at : c - 1;
    n2[j] = name;
    num2[j] = nums == null ? null : nums.clone();
    text2[j] = texts == null ? null : texts.clone();
    types2[j] = null;
    return new Frame(n2, num2, text2, types2, size);
  }

  /** A rolling statistic of a column ({@link Stats#rolling}). */
  public double[] rolling(String column, int window, String stat) {
    return Stats.rolling(numbers(column), window, stat);
  }

  public double[] rolling(String column, int window, String stat, int minPeriods) {
    return Stats.rolling(numbers(column), window, stat, minPeriods);
  }

  /**
   * Multiple linear regression of column y on the x columns, with a constant ({@link
   * LinearModel}): {@code f.ols('price', 'area', 'rooms')}. Rows with a missing value in any of
   * them are left out.
   */
  public LinearModel ols(String y, String... x) {
    if (x == null || x.length == 0) throw new IllegalArgumentException("name at least one x column");
    double[][] cols = new double[x.length][];
    String[] names = new String[x.length];
    for (int j = 0; j < x.length; j++) {
      cols[j] = numbers(x[j]);
      names[j] = this.names[index(x[j])];
    }
    return LinearModel.fit(numbers(y), cols, names);
  }

  /** Principal components of the named columns ({@link Pca}); {@code pca.transform(frame)} scores rows. */
  public Pca pca(int k, String... columns) {
    if (columns == null || columns.length == 0) throw new IllegalArgumentException("name the columns");
    double[][] cols = new double[columns.length][];
    String[] names = new String[columns.length];
    for (int j = 0; j < columns.length; j++) {
      cols[j] = numbers(columns[j]);
      names[j] = this.names[index(columns[j])];
    }
    return Pca.ofColumns(cols, k, names);
  }

  /** The median absolute deviation of a column ({@link Stats#mad}). */
  public double mad(String column) {
    return Stats.mad(numbers(column));
  }

  /** The rows at these positions, in this order. */
  public Frame select(int[] rows) {
    if (rows == null) throw new IllegalArgumentException("the rows must not be null");
    for (int r : rows)
      if (r < 0 || r >= size) throw new IllegalArgumentException("no row " + r + " (" + size + " rows)");
    double[][] n2 = new double[names.length][];
    String[][] t2 = new String[names.length][];
    for (int j = 0; j < names.length; j++) {
      if (num[j] != null) {
        n2[j] = new double[rows.length];
        for (int i = 0; i < rows.length; i++) n2[j][i] = num[j][rows[i]];
      } else {
        t2[j] = new String[rows.length];
        for (int i = 0; i < rows.length; i++) t2[j][i] = text[j][rows[i]];
      }
    }
    return new Frame(names.clone(), n2, t2, types.clone(), rows.length);
  }

  /** {@link #select(int[])} for JavaScript or Python positions. */
  public Frame select(Object rows) {
    long[] l = GuestVectors.longs(rows, "the rows");
    int[] r = new int[l.length];
    for (int i = 0; i < r.length; i++) r[i] = (int) Math.max(-1, Math.min(Integer.MAX_VALUE, l[i]));
    return select(r);
  }

  /** The rows as values (Double or String; missing: null), e.g. for executeMany. */
  public Object[][] rows() {
    Object[][] r = new Object[size][names.length];
    for (int i = 0; i < size; i++)
      for (int j = 0; j < names.length; j++)
        r[i][j] = num[j] != null ? (Double.isNaN(num[j][i]) ? null : (Object) num[j][i]) : text[j][i];
    return r;
  }

  // ------------------------------------------------------------ statistics

  public Summary describe(String column) {
    return Stats.describe(numbers(column));
  }

  public double quantile(String column, double q) {
    return Stats.quantile(numbers(column), q);
  }

  public double median(String column) {
    return Stats.median(numbers(column));
  }

  public Histogram histogram(String column, int bins) {
    return Stats.histogram(numbers(column), bins);
  }

  public int[] outliers(String column, String method, double k) {
    return Stats.outliers(numbers(column), method, k);
  }

  public double[] zscore(String column) {
    return Stats.zscore(numbers(column));
  }

  public double correlation(String x, String y, String method) {
    return Stats.correlation(numbers(x), numbers(y), method);
  }

  public double covariance(String x, String y) {
    return Stats.covariance(numbers(x), numbers(y));
  }

  public Regression linregress(String x, String y) {
    return Stats.linregress(numbers(x), numbers(y));
  }

  /** The correlation of every pair of these columns: a Frame (column, then one column per name). */
  public Frame correlationMatrix(String method, String... columns) {
    if (columns == null || columns.length == 0) throw new IllegalArgumentException("name the columns");
    double[][] cols = new double[columns.length][];
    for (int i = 0; i < cols.length; i++) cols[i] = numbers(columns[i]);
    double[][] m = Stats.correlationMatrix(cols, method);
    String[] n2 = new String[columns.length + 1];
    double[][] num2 = new double[n2.length][];
    String[][] t2 = new String[n2.length][];
    n2[0] = "column";
    t2[0] = columns.clone();
    for (int j = 0; j < columns.length; j++) {
      n2[j + 1] = columns[j];
      num2[j + 1] = new double[columns.length];
      for (int i = 0; i < columns.length; i++) num2[j + 1][i] = m[i][j];
    }
    return new Frame(n2, num2, t2, columns.length);
  }

  // ------------------------------------------------------------ time series

  /**
   * This frame with the times of a DATE, DATETIME or TIMESTAMP column replaced by the start of
   * their bucket (SQL's date_bin, time_bucket, DATE_BUCKET): width "<n> <unit>" with MySQL's
   * INTERVAL units, as '5 MINUTE', '1 HOUR', '1 WEEK', '1 MONTH'. Fixed widths start from
   * 2000-01-03 00:00:00, a Monday (weeks start on Monday, a width that divides a day at midnight);
   * MONTH, QUARTER and YEAR on the first day of a month. Then {@code groupBy(column)} gives
   * statistics per bucket: {@code f.bucket('ts', '5 MINUTE').groupBy('ts').median('price')}.
   */
  public Frame bucket(String column, String width) {
    return FrameTime.bucket(this, column, width, null);
  }

  /** {@link #bucket(String, String)} with the buckets counted from another origin (as '2026-01-01 08:30:00'). */
  public Frame bucket(String column, String width, String origin) {
    if (origin == null) throw new IllegalArgumentException("the origin must not be null");
    return FrameTime.bucket(this, column, width, origin);
  }

  /** A column of numbers put into buckets of a width: floor(x / width) * width (a histogram's bins as rows). */
  public Frame bucket(String column, double width) {
    return FrameTime.bucket(this, column, width);
  }

  /**
   * A time series with no gaps (BigQuery's GAP_FILL): a row for every step from the first time to
   * the last, the steps aligned as {@link #bucket}'s. A row on a step stays as it is; a new row
   * gets its values by method: 'null' leaves them missing, 'locf' carries each column's last value
   * forward, 'linear' interpolates numbers by time between the last and the next value (text: as
   * locf). Rows between steps are not returned but count for locf and linear. Two rows at one
   * time are an error: aggregate first ({@link #bucket}, then {@link #groupBy}).
   */
  public Frame gapFill(String time, String step, String method) {
    return FrameTime.gapFill(this, time, step, method, null);
  }

  /** {@link #gapFill(String, String, String)} for each value of a partition column (one series per sensor, per region). */
  public Frame gapFill(String time, String step, String method, String partition) {
    if (partition == null) throw new IllegalArgumentException("the partition column must not be null");
    return FrameTime.gapFill(this, time, step, method, partition);
  }

  /**
   * The times of a DATE, DATETIME or TIMESTAMP column as seconds since 1970-01-01 00:00:00 on the
   * same clock (missing: NaN), for regressions and differences in time.
   */
  public double[] seconds(String column) {
    return FrameTime.seconds(this, column);
  }

  /**
   * Rows grouped by the values of a column (NULL is a group), groups in ascending order. Text keys
   * are compared exactly, not by the column's collation ({@link Grouped}).
   */
  public Grouped groupBy(String key) {
    return new Grouped(this, index(key));
  }

  // --------------------------------------------------------------- package

  int index(String name) {
    for (int j = 0; j < names.length; j++)
      if (names[j].equals(name)) return j;
    for (int j = 0; j < names.length; j++)
      if (names[j].equalsIgnoreCase(name)) return j;
    throw new IllegalArgumentException("no column '" + name + "' (" + String.join(", ", names) + ")");
  }

  double[] numbers(String name) {
    int j = index(name);
    if (num[j] == null) throw new IllegalArgumentException("column '" + names[j] + "' is not numbers");
    return num[j];
  }

  String name(int j) {
    return names[j];
  }

  Frame replaceText(int j, String[] values) {
    String[][] t2 = text.clone();
    t2[j] = values;
    return new Frame(names.clone(), num.clone(), t2, types.clone(), size);
  }

  Frame replaceNumbers(int j, double[] values) {
    double[][] n2 = num.clone();
    n2[j] = values;
    return new Frame(names.clone(), n2, text.clone(), types.clone(), size);
  }

  /** The same columns and types, other values. */
  Frame withColumns(double[][] num2, String[][] text2, int size2) {
    return new Frame(names.clone(), num2, text2, types.clone(), size2);
  }

  String typeAt(int j) {
    return types[j];
  }

  double[] numbersAt(int j) {
    return num[j];
  }

  String[] textAt(int j) {
    return text[j];
  }

  /** JSON rows: [{"region": "north", "price": 12.5}, ...]; missing values as null. */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < size; i++) {
      b.append(i > 0 ? ", {" : "{");
      for (int j = 0; j < names.length; j++) {
        if (j > 0) b.append(", ");
        quote(b, names[j]).append(": ");
        if (num[j] != null) b.append(Stats.json(num[j][i]));
        else if (text[j][i] == null) b.append("null");
        else quote(b, text[j][i]);
      }
      b.append('}');
    }
    return b.append(']').toString();
  }

  static StringBuilder quote(StringBuilder b, String s) {
    b.append('"');
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c == '"' || c == '\\') b.append('\\').append(c);
      else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
      else b.append(c);
    }
    return b.append('"');
  }
}
