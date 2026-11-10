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

package com.percona.mle.sql;

import com.percona.mle.worker.SqlBridge;
import java.security.SecureRandom;
import java.util.concurrent.ThreadLocalRandom;
import java.util.regex.Pattern;

/**
 * Oracle's mysql.rand/sleep/uuid/isUUID. They run in the worker, so they also work in stored
 * functions (where SQL is not allowed).
 */
public final class Functions {
  private Functions() {}

  /** Like RAND(): 0 <= v < 1. */
  public static double rand() {
    return ThreadLocalRandom.current().nextDouble();
  }

  /** Like SLEEP(): 0, or 1 if the statement was killed meanwhile. */
  public static int sleep(double seconds) {
    if (!(seconds > 0)) return 0;
    long end = System.nanoTime() + (long) Math.min(seconds * 1e9, Long.MAX_VALUE / 2.0);
    try {
      for (long left = end - System.nanoTime(); left > 0; left = end - System.nanoTime()) {
        if (SqlBridge.interrupted()) return 1;
        Thread.sleep(Math.min(left / 1_000_000 + 1, 10));
      }
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
      return 1;
    }
    return 0;
  }

  private static final long GREGORIAN_OFFSET = 0x01B21DD213814000L;  // 100 ns units, 1582 to 1970
  private static final SecureRandom RANDOM = new SecureRandom();
  private static final long NODE = (RANDOM.nextLong() & 0xFFFFFFFFFFFFL) | 0x010000000000L;  // multicast bit
  private static final int CLOCK_SEQ = RANDOM.nextInt(1 << 14);
  private static long last;

  /** Like UUID(): a version 1 UUID (time based; a random node id). */
  public static synchronized String uuid() {
    long t = System.currentTimeMillis() * 10_000 + GREGORIAN_OFFSET;
    if (t <= last) t = last + 1;
    last = t;
    long hi = ((t & 0xFFFFFFFFL) << 32) | (((t >>> 32) & 0xFFFF) << 16) | 0x1000 | ((t >>> 48) & 0x0FFF);
    long lo = ((long) (0x8000 | CLOCK_SEQ) << 48) | NODE;
    String h = String.format("%016x%016x", hi, lo);
    return h.substring(0, 8) + "-" + h.substring(8, 12) + "-" + h.substring(12, 16) + "-" + h.substring(16, 20) + "-"
        + h.substring(20);
  }

  private static final Pattern UUID = Pattern.compile(
      "[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
      + "|\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}}");

  /** Like IS_UUID(): 1 or 0, null for null. */
  public static Integer isUUID(String s) {
    return s == null ? null : UUID.matcher(s).matches() ? 1 : 0;
  }
}
