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
 */
public final class Frame {
  private final String[] names;
  private final double[][] num;  // per column, null for text
  private final String[][] text;  // per column, null for numbers
  private final int size;

  Frame(String[] names, double[][] num, String[][] text, int size) {
    this.names = names;
    this.num = num;
    this.text = text;
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
      }
    }
    return new Frame(names, num, text, n);
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

  /** A value of a text column as MySQL prints it (decimals: the column's fractional seconds). */
  private static String asText(Object v, int decimals) {
    if (v instanceof byte[] b) return new String(b, StandardCharsets.UTF_8);
    if (v instanceof float[] f) return Arrays.toString(f);  // VECTOR
    if (v instanceof Duration d) {  // TIME: [-]HH:MM:SS, hours up to 838
      Duration a = d.isNegative() ? d.negated() : d;
      long s = a.getSeconds();
      return (d.isNegative() ? "-" : "") + clock(s / 3600, (int) (s / 60 % 60), (int) (s % 60), a.getNano(), decimals);
    }
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
    return new Frame(names.clone(), n2, t2, rows.length);
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

  private static StringBuilder quote(StringBuilder b, String s) {
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
