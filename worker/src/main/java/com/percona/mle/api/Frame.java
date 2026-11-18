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

import com.percona.mle.sql.Row;
import com.percona.mle.sql.SqlResult;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Columns from SQL kept in Java ({@link Stats#load}), for statistics per column and per group
 * (exp note 54): numbers as double (NULL: NaN, missing), anything else as text (NULL: null).
 * Results that are tables (per group, a correlation matrix, a selection of rows) are Frames too;
 * {@code String(t)} / {@code str(t)} gives JSON rows, {@link #rows()} rows for executeMany.
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
    SqlResult r = Mle.session().runSql(sql, params == null ? new Object[0] : params);
    String[] names = r.getColumnNames().toArray(new String[0]);
    int c = names.length;
    List<Object[]> rows = new ArrayList<>();
    for (Row row : r) rows.add(row.toArray());
    int n = rows.size();
    double[][] num = new double[c][];
    String[][] text = new String[c][];
    for (int j = 0; j < c; j++) {
      boolean numbers = true;
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
          t[i] = v == null ? null : v instanceof byte[] b ? new String(b, StandardCharsets.UTF_8) : String.valueOf(v);
        }
      }
    }
    return new Frame(names, num, text, n);
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

  /** Rows grouped by the values of a column (NULL is a group), groups in ascending order. */
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
