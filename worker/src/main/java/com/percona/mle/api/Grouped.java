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

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A {@link Frame}'s rows grouped by a column ({@link Frame#groupBy}): statistics per group that
 * SQL's GROUP BY cannot compute in MySQL (median, quantiles, a summary, correlation). Each result
 * is a Frame: the key, then the statistic(s); groups in ascending order of the key, NULL first.
 */
public final class Grouped {
  private final Frame t;
  private final int key;
  private final int[][] members;  // per group, row positions in order
  private final double[] numKeys;  // numeric key per group, or null
  private final String[] textKeys;  // text key per group, or null

  Grouped(Frame t, int key) {
    this.t = t;
    this.key = key;
    double[] nk = t.numbersAt(key);
    String[] tk = t.textAt(key);
    int n = t.size();
    Map<Object, List<Integer>> by = new HashMap<>();
    for (int i = 0; i < n; i++) {
      Object k = nk != null ? (Object) (Double.isNaN(nk[i]) ? null : nk[i]) : tk[i];
      by.computeIfAbsent(k == null ? NULL : k, x -> new ArrayList<>()).add(i);
    }
    List<Object> keys = new ArrayList<>(by.keySet());
    keys.sort((a, b) -> {
      if (a == b) return 0;
      if (a == NULL) return -1;
      if (b == NULL) return 1;
      return nk != null ? Double.compare((Double) a, (Double) b) : ((String) a).compareTo((String) b);
    });
    members = new int[keys.size()][];
    numKeys = nk != null ? new double[keys.size()] : null;
    textKeys = nk == null ? new String[keys.size()] : null;
    for (int g = 0; g < keys.size(); g++) {
      Object k = keys.get(g);
      members[g] = by.get(k).stream().mapToInt(Integer::intValue).toArray();
      if (nk != null) numKeys[g] = k == NULL ? Double.NaN : (Double) k;
      else textKeys[g] = k == NULL ? null : (String) k;
    }
  }

  private static final Object NULL = new Object();

  /** The number of groups. */
  public int size() {
    return members.length;
  }

  /** Rows per group. */
  public Frame count() {
    double[] c = new double[members.length];
    for (int g = 0; g < c.length; g++) c[g] = members[g].length;
    return result(new String[] {"count"}, new double[][] {c});
  }

  public Frame mean(String column) {
    return per(column, x -> StatsMath.mean(StatsMath.sorted(x)));
  }

  public Frame median(String column) {
    return per(column, x -> StatsMath.quantileSorted(StatsMath.sorted(x), 0.5));
  }

  public Frame quantile(String column, double q) {
    StatsMath.checkQ(q);
    return per(column, x -> StatsMath.quantileSorted(StatsMath.sorted(x), q));
  }

  /** A Summary per group, as columns count, missing, mean, std, min, q1, median, q3, max, skewness, kurtosis. */
  public Frame describe(String column) {
    double[] x = t.numbers(column);
    String[] names = {"count", "missing", "mean", "std", "min", "q1", "median", "q3", "max", "skewness", "kurtosis"};
    double[][] cols = new double[names.length][members.length];
    for (int g = 0; g < members.length; g++) {
      Summary s = Summary.of(pick(x, members[g]));
      double[] v = {s.count(), s.missing(), s.mean(), s.std(), s.min(), s.q1(), s.median(), s.q3(), s.max(),
          s.skewness(), s.kurtosis()};
      for (int j = 0; j < v.length; j++) cols[j][g] = v[j];
    }
    return result(names, cols);
  }

  public Frame correlation(String x, String y, String method) {
    double[] a = t.numbers(x), b = t.numbers(y);
    double[] c = new double[members.length];
    for (int g = 0; g < c.length; g++) c[g] = StatsMath.correlation(pick(a, members[g]), pick(b, members[g]), method);
    return result(new String[] {"correlation"}, new double[][] {c});
  }

  private Frame per(String column, java.util.function.ToDoubleFunction<double[]> f) {
    double[] x = t.numbers(column);
    double[] c = new double[members.length];
    for (int g = 0; g < c.length; g++) c[g] = f.applyAsDouble(pick(x, members[g]));
    return result(new String[] {t.name(t.index(column))}, new double[][] {c});
  }

  private static double[] pick(double[] x, int[] rows) {
    double[] r = new double[rows.length];
    for (int i = 0; i < r.length; i++) r[i] = x[rows[i]];
    return r;
  }

  private Frame result(String[] names, double[][] cols) {
    String[] n2 = new String[names.length + 1];
    double[][] num = new double[n2.length][];
    String[][] text = new String[n2.length][];
    n2[0] = t.name(key);
    if (numKeys != null) num[0] = numKeys.clone();
    else text[0] = textKeys.clone();
    for (int j = 0; j < names.length; j++) {
      n2[j + 1] = names[j];
      num[j + 1] = cols[j];
    }
    return new Frame(n2, num, text, members.length);
  }

  @Override
  public String toString() {
    return "Grouped(" + members.length + " groups by " + t.name(key) + ")";
  }
}
