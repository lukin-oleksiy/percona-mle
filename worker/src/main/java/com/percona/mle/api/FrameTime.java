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
import java.util.Locale;
import java.util.Map;

/** {@link Frame#bucket}, {@link Frame#gapFill}, {@link Stats#timeSeries} (exp note 75). Not API. */
final class FrameTime {
  private FrameTime() {}

  /** More rows than this from a series or a gap fill is an error (the step is too small for the range). */
  static final int MAX_STEPS = 1_000_000;

  /** The steps from the bucket of first to last; an error when there are more than MAX_STEPS. */
  static long steps(long first, long last, StatsTime.Width w, String what) {
    long n;
    if (w.calendar()) {
      long months = monthsBetween(first, last);
      n = months / w.months() + 1;
    } else {
      long a = StatsTime.bucket(first, w, StatsTime.ORIGIN);
      n = (last - a) / w.micros() + 1;  // last - a fits: both are times within years 0 to 9999
    }
    if (n > MAX_STEPS)
      throw new IllegalArgumentException("more than " + MAX_STEPS + " steps of " + w.text() + " " + what
          + ": a larger step, or a shorter range");
    return n;
  }

  private static long monthsBetween(long a, long b) {
    java.time.LocalDate x = java.time.LocalDate.ofEpochDay(Math.floorDiv(a, StatsTime.US_PER_DAY)),
        y = java.time.LocalDate.ofEpochDay(Math.floorDiv(b, StatsTime.US_PER_DAY));
    return (y.getYear() * 12L + y.getMonthValue()) - (x.getYear() * 12L + x.getMonthValue());
  }

  static int timeColumn(Frame f, String name) {
    int j = f.index(name);
    if (f.typeAt(j) == null)
      throw new IllegalArgumentException("column '" + f.name(j) + "' is not a DATE, DATETIME or TIMESTAMP"
          + " (CAST(... AS DATETIME) in the query makes it one)");
    return j;
  }

  static Frame bucket(Frame f, String column, String width, String origin) {
    int j = timeColumn(f, column);
    StatsTime.Width w = StatsTime.width(width);
    long o = origin == null ? StatsTime.ORIGIN : StatsTime.parse(origin);
    String type = f.typeAt(j);
    boolean dateOnly = StatsTime.dateOnly(type);
    int digits = StatsTime.digits(type);
    String[] t = f.textAt(j), out = new String[t.length];
    for (int i = 0; i < t.length; i++)
      out[i] = t[i] == null ? null : StatsTime.format(StatsTime.bucket(StatsTime.parse(t[i]), w, o), dateOnly, digits);
    return f.replaceText(j, out);
  }

  static Frame bucket(Frame f, String column, double width) {
    if (!(width > 0) || Double.isInfinite(width))
      throw new IllegalArgumentException("the width must be a positive number, not " + width);
    int j = f.index(column);
    double[] x = f.numbersAt(j);
    if (x == null) throw new IllegalArgumentException("column '" + f.name(j) + "' is not numbers");
    double[] out = new double[x.length];
    for (int i = 0; i < x.length; i++) out[i] = Math.floor(x[i] / width) * width + 0.0;
    return f.replaceNumbers(j, out);
  }

  static double[] seconds(Frame f, String column) {
    int j = timeColumn(f, column);
    String[] t = f.textAt(j);
    double[] s = new double[t.length];
    for (int i = 0; i < t.length; i++) {
      long us = StatsTime.parse(t[i]);
      s[i] = us == Long.MIN_VALUE ? Double.NaN : Math.floorDiv(us, 1_000_000L) + Math.floorMod(us, 1_000_000L) / 1e6;
    }
    return s;
  }

  static String[] series(String start, String stop, String step) {
    long a = StatsTime.parse(start), b = StatsTime.parse(stop);
    if (a == Long.MIN_VALUE || b == Long.MIN_VALUE) throw new IllegalArgumentException("start and stop must not be null");
    StatsTime.Width w = StatsTime.width(step);
    if (b >= a) steps(a, b, w, "from " + start + " to " + stop);
    boolean dateOnly = start.trim().length() == 10 && (w.calendar() || w.micros() % StatsTime.US_PER_DAY == 0);
    int digits = Math.max(fraction(start), Math.max(fraction(stop),
        w.calendar() || w.micros() % 1_000_000 == 0 ? 0 : w.micros() % 1000 == 0 ? 3 : 6));
    List<String> out = new ArrayList<>();
    // The k-th time is start + k steps (not the previous one + a step: from 31 January by a month,
    // 28 February, then 31 March, as PostgreSQL's generate_series).
    for (long k = 0, t = a; t <= b; k++, t = w.calendar() ? StatsTime.plusMonths(a, k * w.months()) : a + k * w.micros()) {
      if (out.size() >= MAX_STEPS)
        throw new IllegalArgumentException("more than " + MAX_STEPS + " steps of " + step + " from " + start + " to " + stop);
      out.add(StatsTime.format(t, dateOnly, digits));
    }
    return out.toArray(new String[0]);
  }

  private static int fraction(String s) {
    int dot = s.indexOf('.');
    return dot < 0 ? 0 : s.trim().length() - dot - 1;
  }

  // -------------------------------------------------------------- gap fill

  static Frame gapFill(Frame f, String time, String step, String method, String partition) {
    int tj = timeColumn(f, time);
    int pj = partition == null ? -1 : f.index(partition);
    if (pj == tj) throw new IllegalArgumentException("the partition column cannot be the time column");
    String m = method == null ? "" : method.trim().toLowerCase(Locale.ROOT);
    if (!m.equals("null") && !m.equals("locf") && !m.equals("linear"))
      throw new IllegalArgumentException("unknown method '" + method + "' (null, locf or linear)");
    StatsTime.Width w = StatsTime.width(step);
    String type = f.typeAt(tj);
    boolean dateOnly = StatsTime.dateOnly(type);
    int digits = StatsTime.digits(type);
    int n = f.size(), c = f.columns().length;

    // Rows by partition (NULL first, then ascending, as groupBy), rows with a NULL time left out.
    String[] tt = f.textAt(tj);
    long[] us = new long[n];
    for (int i = 0; i < n; i++) us[i] = StatsTime.parse(tt[i]);
    Map<Object, List<Integer>> by = new HashMap<>();
    double[] pn = pj < 0 ? null : f.numbersAt(pj);
    String[] pt = pj < 0 ? null : f.textAt(pj);
    Object nullKey = new Object();
    for (int i = 0; i < n; i++) {
      if (us[i] == Long.MIN_VALUE) continue;
      Object k = pj < 0 ? nullKey : pn != null ? (Double.isNaN(pn[i]) ? nullKey : (Object) (pn[i] + 0.0))
          : pt[i] == null ? nullKey : pt[i];
      by.computeIfAbsent(k, x -> new ArrayList<>()).add(i);
    }
    List<Object> keys = new ArrayList<>(by.keySet());
    keys.sort((a, b) -> a == b ? 0 : a == nullKey ? -1 : b == nullKey ? 1
        : a instanceof Double da ? Double.compare(da, (Double) b) : ((String) a).compareTo((String) b));

    long total = 0;  // the rows to fill, before any is made
    for (Object k : keys) {
      long lo = Long.MAX_VALUE, hi = Long.MIN_VALUE;
      for (int i : by.get(k)) {
        lo = Math.min(lo, us[i]);
        hi = Math.max(hi, us[i]);
      }
      total += steps(lo, hi, w, "to fill");
      if (total > MAX_STEPS)
        throw new IllegalArgumentException("more than " + MAX_STEPS + " rows to fill with steps of " + w.text()
            + ": a larger step, or fewer rows");
    }
    Out out = new Out(f, c);
    for (Object k : keys) {
      int[] rows = by.get(k).stream().mapToInt(Integer::intValue).toArray();
      sortByTime(rows, us);
      for (int r = 1; r < rows.length; r++)
        if (us[rows[r]] == us[rows[r - 1]])
          throw new IllegalArgumentException("two rows at " + tt[rows[r]] + (pj < 0 ? "" : " in partition "
              + (k == nullKey ? "NULL" : k)) + ": aggregate first (bucket, then groupBy)");
      fillPartition(f, rows, us, w, m, tj, pj, dateOnly, digits, out);
    }
    return out.frame(f);
  }

  private static void fillPartition(Frame f, int[] rows, long[] us, StatsTime.Width w, String m, int tj, int pj,
      boolean dateOnly, int digits, Out out) {
    int n = rows.length, c = out.cols;
    // per column, the last and the next row (in time order) with a value: for locf and linear
    int[][] prev = new int[c][], next = new int[c][];
    if (!m.equals("null"))
      for (int j = 0; j < c; j++) {
        if (j == tj || j == pj) continue;
        int[] p = prev[j] = new int[n], q = next[j] = new int[n];
        for (int r = 0, last = -1; r < n; r++) {
          if (present(f, j, rows[r])) last = r;
          p[r] = last;
        }
        for (int r = n - 1, nx = -1; r >= 0; r--) {
          if (present(f, j, rows[r])) nx = r;
          q[r] = nx;
        }
      }
    long first = us[rows[0]], last = us[rows[n - 1]];
    int at = 0;  // rows before at are at or before the current step
    for (long g = StatsTime.bucket(first, w, StatsTime.ORIGIN); g <= last; g = StatsTime.next(g, w)) {
      if (g < first) continue;
      while (at < n && us[rows[at]] <= g) at++;
      if (at > 0 && us[rows[at - 1]] == g) {  // a row on the step: as it is
        out.copy(f, rows[at - 1]);
        continue;
      }
      out.add();
      out.setTime(tj, StatsTime.format(g, dateOnly, digits));
      if (pj >= 0) out.set(f, pj, rows[0]);
      for (int j = 0; j < c; j++) {
        if (j == tj || j == pj || m.equals("null")) continue;
        int before = at > 0 ? prev[j][at - 1] : -1;
        if (before < 0) continue;  // nothing to carry or interpolate from: missing
        double[] x = f.numbersAt(j);
        if (m.equals("locf") || x == null) {
          out.set(f, j, rows[before]);
          continue;
        }
        int after = at < n ? next[j][at] : -1;
        if (after < 0) continue;  // after the last value: missing
        long t0 = us[rows[before]], t1 = us[rows[after]];
        double v0 = x[rows[before]], v1 = x[rows[after]];
        out.setNumber(j, v0 + (v1 - v0) * ((double) (g - t0) / (double) (t1 - t0)));
      }
    }
  }

  private static boolean present(Frame f, int j, int row) {
    double[] x = f.numbersAt(j);
    return x != null ? !Double.isNaN(x[row]) : f.textAt(j)[row] != null;
  }

  /** Sorts row positions by their times (stable merge sort on long keys, no boxing). */
  private static void sortByTime(int[] idx, long[] key) {
    int[] tmp = new int[idx.length];
    for (int width = 1; width < idx.length; width *= 2) {
      for (int lo = 0; lo < idx.length; lo += 2 * width) {
        int mid = Math.min(lo + width, idx.length), hi = Math.min(lo + 2 * width, idx.length);
        int a = lo, b = mid, o = lo;
        while (a < mid && b < hi) tmp[o++] = key[idx[b]] < key[idx[a]] ? idx[b++] : idx[a++];
        while (a < mid) tmp[o++] = idx[a++];
        while (b < hi) tmp[o++] = idx[b++];
      }
      System.arraycopy(tmp, 0, idx, 0, idx.length);
    }
  }

  /** The rows of a gap-filled frame, built column by column. */
  private static final class Out {
    final int cols;
    final double[][] num;
    final String[][] text;
    int size;

    Out(Frame f, int c) {
      cols = c;
      num = new double[c][];
      text = new String[c][];
      for (int j = 0; j < c; j++) {
        if (f.numbersAt(j) != null) num[j] = new double[16];
        else text[j] = new String[16];
      }
    }

    void add() {
      if (size == (num[0] != null ? num[0].length : text[0].length))
        for (int j = 0; j < cols; j++) {
          if (num[j] != null) num[j] = Arrays.copyOf(num[j], size * 2);
          else text[j] = Arrays.copyOf(text[j], size * 2);
        }
      for (int j = 0; j < cols; j++)
        if (num[j] != null) num[j][size] = Double.NaN;
      size++;
    }

    void copy(Frame f, int row) {
      add();
      for (int j = 0; j < cols; j++) set(f, j, row);
    }

    void set(Frame f, int j, int row) {
      if (num[j] != null) num[j][size - 1] = f.numbersAt(j)[row];
      else text[j][size - 1] = f.textAt(j)[row];
    }

    void setNumber(int j, double v) {
      num[j][size - 1] = v;
    }

    void setTime(int j, String t) {
      text[j][size - 1] = t;
    }

    Frame frame(Frame f) {
      for (int j = 0; j < cols; j++) {
        if (num[j] != null) num[j] = Arrays.copyOf(num[j], size);
        else text[j] = Arrays.copyOf(text[j], size);
      }
      return f.withColumns(num, text, size);
    }
  }
}
