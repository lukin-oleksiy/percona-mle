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

import java.time.LocalDate;
import java.util.Locale;

/**
 * Times for the statistics helper (exp note 75): MySQL's DATE, DATETIME and TIMESTAMP text
 * ("2026-11-27", "2026-11-27 10:05:00.123") as microseconds since 1970-01-01 00:00 on the wall
 * clock (no time zone: TIMESTAMP values are in the session's zone, as MySQL returned them), widths
 * and steps as "<n> <unit>" with MySQL's INTERVAL units. Not API.
 *
 * <p>Fixed widths (MICROSECOND to WEEK) are aligned to an origin, by default 2000-01-03 00:00:00,
 * a Monday (as TimescaleDB's time_bucket): weeks start on Monday, and a width that divides a day
 * starts at midnight. MONTH, QUARTER and YEAR are calendar widths: buckets start on the first day
 * of a month, counted from the origin's month.
 */
final class StatsTime {
  private StatsTime() {}

  static final long US_PER_DAY = 86_400_000_000L;
  /** 2000-01-03 00:00:00, a Monday. */
  static final long ORIGIN = LocalDate.of(2000, 1, 3).toEpochDay() * US_PER_DAY;

  /** A width or step: micros > 0 for a fixed one, else months > 0. */
  record Width(long micros, int months, String text) {
    boolean calendar() {
      return months > 0;
    }
  }

  static Width width(String s) {
    if (s == null) throw new IllegalArgumentException("the width must not be null (as '5 MINUTE')");
    String[] p = s.trim().split("\\s+");
    if (p.length != 2)
      throw new IllegalArgumentException("'" + s + "': a width is a number and a unit, as '5 MINUTE' or '1 DAY'");
    long n;
    try {
      n = Long.parseLong(p[0]);
    } catch (NumberFormatException e) {
      throw new IllegalArgumentException("'" + s + "': the number must be a whole number, as '15 MINUTE'");
    }
    if (n < 1) throw new IllegalArgumentException("'" + s + "': the width must be at least 1");
    String u = p[1].toUpperCase(Locale.ROOT);
    if (u.endsWith("S") && !u.equals("S")) u = u.substring(0, u.length() - 1);
    long unit;
    int months = 0;
    switch (u) {
      case "MICROSECOND" -> unit = 1;
      case "MILLISECOND" -> unit = 1000;
      case "SECOND" -> unit = 1_000_000;
      case "MINUTE" -> unit = 60_000_000;
      case "HOUR" -> unit = 3_600_000_000L;
      case "DAY" -> unit = US_PER_DAY;
      case "WEEK" -> unit = 7 * US_PER_DAY;
      case "MONTH" -> { unit = 0; months = 1; }
      case "QUARTER" -> { unit = 0; months = 3; }
      case "YEAR" -> { unit = 0; months = 12; }
      default -> throw new IllegalArgumentException("'" + s + "': unknown unit '" + p[1]
          + "' (MICROSECOND, MILLISECOND, SECOND, MINUTE, HOUR, DAY, WEEK, MONTH, QUARTER, YEAR)");
    }
    if (months > 0) {
      if (n > 12_000) throw new IllegalArgumentException("'" + s + "': the width is too large");
      return new Width(0, (int) (n * months), s.trim());
    }
    if (n > Long.MAX_VALUE / unit / 4) throw new IllegalArgumentException("'" + s + "': the width is too large");
    return new Width(n * unit, 0, s.trim());
  }

  /** MySQL's text of a DATE, DATETIME or TIMESTAMP (also with a T) as micros; Long.MIN_VALUE for null. */
  static long parse(String s) {
    if (s == null) return Long.MIN_VALUE;
    String t = s.trim();
    try {
      int y = Integer.parseInt(t.substring(0, 4)), mo = Integer.parseInt(t.substring(5, 7)),
          d = Integer.parseInt(t.substring(8, 10));
      if (t.charAt(4) != '-' || t.charAt(7) != '-') throw new NumberFormatException();
      long micros = LocalDate.of(y, mo, d).toEpochDay() * US_PER_DAY;
      if (t.length() == 10) return micros;
      char sep = t.charAt(10);
      if ((sep != ' ' && sep != 'T') || t.charAt(13) != ':' || t.charAt(16) != ':') throw new NumberFormatException();
      int h = Integer.parseInt(t.substring(11, 13)), mi = Integer.parseInt(t.substring(14, 16)),
          se = Integer.parseInt(t.substring(17, 19));
      if (h > 23 || mi > 59 || se > 59) throw new NumberFormatException();
      long frac = 0;
      if (t.length() > 19) {
        if (t.charAt(19) != '.' || t.length() > 26 || t.length() == 20) throw new NumberFormatException();
        String f = (t.substring(20) + "000000").substring(0, 6);
        frac = Long.parseLong(f);
      }
      return micros + ((h * 60L + mi) * 60 + se) * 1_000_000 + frac;
    } catch (RuntimeException e) {
      throw new IllegalArgumentException("'" + s + "' is not a date or time as MySQL writes it"
          + " ('2026-11-27' or '2026-11-27 10:05:00')");
    }
  }

  /** Micros as MySQL's text: a date, or a date and time with digits fractional digits (0 to 6). */
  static String format(long micros, boolean dateOnly, int digits) {
    long day = Math.floorDiv(micros, US_PER_DAY), us = Math.floorMod(micros, US_PER_DAY);
    String d = LocalDate.ofEpochDay(day).toString();
    if (dateOnly) return d;
    long s = us / 1_000_000;
    String t = String.format(Locale.ROOT, "%s %02d:%02d:%02d", d, s / 3600, s / 60 % 60, s % 60);
    if (digits <= 0) return t;
    return t + "." + String.format(Locale.ROOT, "%06d", us % 1_000_000).substring(0, Math.min(6, digits));
  }

  /** The start of the bucket of t. */
  static long bucket(long t, Width w, long origin) {
    if (!w.calendar()) return Math.floorDiv(t - origin, w.micros()) * w.micros() + origin;
    long m = monthIndex(t) - monthIndex(origin);
    return monthStart(monthIndex(origin) + Math.floorDiv(m, w.months()) * w.months());
  }

  /** The next step after t (t a bucket start). */
  static long next(long t, Width w) {
    return w.calendar() ? monthStart(monthIndex(t) + w.months()) : t + w.micros();
  }

  /** t plus months on the calendar, the day clamped to the month's last (PostgreSQL's interval arithmetic). */
  static long plusMonths(long t, long months) {
    long day = Math.floorDiv(t, US_PER_DAY), us = Math.floorMod(t, US_PER_DAY);
    return LocalDate.ofEpochDay(day).plusMonths(months).toEpochDay() * US_PER_DAY + us;
  }

  /** Months since year 0 of the month of t. */
  private static long monthIndex(long t) {
    LocalDate d = LocalDate.ofEpochDay(Math.floorDiv(t, US_PER_DAY));
    return d.getYear() * 12L + d.getMonthValue() - 1;
  }

  private static long monthStart(long monthIndex) {
    return LocalDate.of((int) Math.floorDiv(monthIndex, 12), Math.floorMod(monthIndex, 12) + 1, 1).toEpochDay()
        * US_PER_DAY;
  }

  /** "DATE", "DATETIME", "TIMESTAMP", with "(d)" for fractional digits: whether it is a time type. */
  static boolean isTime(String type) {
    return type != null;
  }

  static boolean dateOnly(String type) {
    return type != null && type.startsWith("DATE") && !type.startsWith("DATETIME");
  }

  static int digits(String type) {
    int i = type == null ? -1 : type.indexOf('(');
    return i < 0 ? 0 : Integer.parseInt(type.substring(i + 1, type.length() - 1));
  }
}
