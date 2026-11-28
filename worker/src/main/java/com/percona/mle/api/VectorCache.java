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

import java.math.BigDecimal;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.List;
import org.graalvm.polyglot.Value;

/**
 * The worker's vector cache (exp note 76): sets kept between the calls of a session under names a
 * routine chooses ({@link Vectors#cached}, {@link Vectors#keep}, ...). A name belongs to the calling
 * code's schema (the routine's; for code run directly, the current schema). The cache is a snapshot
 * (writes after the load are not in a kept set), lives in this worker (one principal) until the
 * session ends (RESET), and is limited by mle.vector_cache_max (bytes; 0: off), least recently used
 * set evicted first; a set unused for its idle time (mle.vector_cache_idle_ms, or {@link
 * Vectors#expireAfter}) is dropped.
 *
 * <p>The public static methods without a counterpart in {@link Vectors} are for the worker's
 * dispatcher (RESET, the idle wait), not for routines.
 */
public final class VectorCache {
  private VectorCache() {}

  /**
   * The limit: mle.vector_cache_max (-Dmle.vectorCacheMax), -1 (the default) for a quarter of the
   * heap; at most the heap (a larger setting would never evict before the heap is exhausted).
   */
  static final long MAX = limit(Long.getLong("mle.vectorCacheMax", -1));
  /** The default idle time: mle.vector_cache_idle_ms (-Dmle.vectorCacheIdleMs), 30 minutes; 0: never. */
  static final long IDLE_MS = Math.max(0, Long.getLong("mle.vectorCacheIdleMs", 1_800_000L));

  static final CacheTable TABLE = new CacheTable(MAX, IDLE_MS * 1_000_000L, System::nanoTime);

  static long limit(long setting) {
    long heap = Runtime.getRuntime().maxMemory();
    return setting < 0 ? heap / 4 : Math.min(setting, heap);
  }

  // ------------------------------------------------------- for the library

  static CacheTable.Key key(String name) {
    if (name == null || name.isEmpty()) throw new IllegalArgumentException("a cached set needs a name");
    String schema = com.percona.mle.worker.SqlBridge.routineSchema();
    return new CacheTable.Key(schema == null ? "" : schema, name);
  }

  static String kind(Object set) {
    return set instanceof QuantizedSet q ? q.type() : "vectors";
  }

  static VectorSet cached(String name, String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    Object[] p = javaValues(params);
    return (VectorSet) TABLE.cached(key(name), "vectors", sql, values(p), p, () -> sized(VectorSet.load(sql, p)),
        com.percona.mle.worker.SqlBridge.maySendSql()).set();
  }

  static QuantizedSet cachedQuantized(String name, String type, String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    String kind = QuantizedSet.binaryType(type) ? "binary" : "int8";
    Object[] p = javaValues(params);
    return (QuantizedSet) TABLE.cached(key(name), kind, sql, values(p), p,
        () -> sized(QuantizedSet.load(kind, sql, p)), com.percona.mle.worker.SqlBridge.maySendSql()).set();
  }

  static Object refresh(String name) {
    CacheTable.Sized v = TABLE.refresh(key(name), e -> {
      Object[] p = e.raw;
      return e.kind.equals("vectors") ? sized(VectorSet.load(e.sql, p)) : sized(QuantizedSet.load(e.kind, e.sql, p));
    });
    return v == null ? null : v.set();
  }

  /** What the table needs to know of a set (its bytes, what it shares), without making it API (T6). */
  static CacheTable.Sized sized(VectorSet s) {
    return new CacheTable.Sized() {
      public Object set() { return s; }
      public Object base() { return s.cacheBase(); }
      public long baseBytes() { return s.cacheBaseBytes(); }
      public long ownBytes() { return s.cacheOwnBytes(); }
      public int rows() { return s.size(); }
      public int dimensions() { return s.dimensions(); }
    };
  }

  static CacheTable.Sized sized(QuantizedSet q) {
    return new CacheTable.Sized() {
      public Object set() { return q; }
      public Object base() { return q.cacheBase(); }
      public long baseBytes() { return q.cacheBaseBytes(); }
      public long ownBytes() { return q.cacheOwnBytes(); }
      public int rows() { return q.size(); }
      public int dimensions() { return q.dimensions(); }
    };
  }

  /**
   * The parameters as Java values, taken once: a JavaScript array or object changed after the call
   * does not change the kept query, and no guest object is held after the call. A guest number
   * becomes Long or Double, text String, a guest date and time LocalDate, LocalTime, LocalDateTime
   * or Instant, a duration Duration, an array or numpy array float[] (a VECTOR); a Java array is
   * copied; other Java objects stay as they are. A guest value that is none of these stays as it is
   * (the load then reports it as Vectors.load does).
   */
  static Object[] javaValues(Object[] params) {
    if (params == null) return new Object[0];
    Object[] out = new Object[params.length];
    for (int i = 0; i < out.length; i++) out[i] = javaValue(params[i], i);
    return out;
  }

  private static Object javaValue(Object p, int i) {
    if (p == null || p instanceof Number || p instanceof String || p instanceof Boolean || p instanceof MleJson
        || p instanceof java.time.temporal.TemporalAccessor || p instanceof java.time.Duration) return p;
    if (p instanceof float[] f) return f.clone();
    if (p instanceof byte[] b) return b.clone();
    if (p instanceof double[] d) return d.clone();
    if (p.getClass().isArray()) return p;  // not a parameter SQL takes: the load says so
    // A guest value: as a Value, or wrapped for Java (a JavaScript array arrives as a List, an object as a Map).
    Value v = p instanceof Value pv ? pv : Value.asValue(p);
    if (v.isHostObject()) {
      Object h = v.asHostObject();
      return h == p ? p : javaValue(h, i);
    }
    if (v.isNull()) return null;
    if (v.isBoolean()) return v.asBoolean();
    if (v.isNumber()) return v.fitsInLong() ? (Object) v.asLong() : (Object) v.asDouble();
    if (v.isString()) return v.asString();
    if (v.isInstant() && v.isTimeZone()) return v.asInstant();
    if (v.isDate() && v.isTime()) return java.time.LocalDateTime.of(v.asDate(), v.asTime());
    if (v.isDate()) return v.asDate();
    if (v.isTime()) return v.asTime();
    if (v.isDuration()) return v.asDuration();
    if (v.hasArrayElements() || v.hasMember("tobytes")) return GuestVectors.vector(v, "parameter " + i);
    return p;
  }

  /**
   * The parameters (Java values, from javaValues) as the values they become in SQL, to compare by
   * value: integers as Long (or BigInteger), other numbers as Double, or a BigDecimal without
   * trailing zeros when no double is that number; text as String, a VECTOR or bytes as a list of
   * their elements, JSON as its text, a date or time as itself; anything else as itself (equal only
   * to itself, or by its equals).
   */
  static List<Object> values(Object[] params) {
    List<Object> out = new ArrayList<>(params.length);
    for (Object p : params) out.add(value(p));
    return List.copyOf(out.stream().map(v -> v == null ? (Object) NULL : v).toList());
  }

  /** A stand-in for a NULL parameter (List.copyOf takes no nulls). */
  private static final String NULL = "\u0000NULL";

  private static Object value(Object p) {
    if (p == null) return null;
    if (p instanceof Long || p instanceof Integer || p instanceof Short || p instanceof Byte) return ((Number) p).longValue();
    if (p instanceof Boolean b) return b ? 1L : 0L;
    if (p instanceof Double || p instanceof Float) return number(((Number) p).doubleValue());
    if (p instanceof BigInteger b) return b.bitLength() < 64 ? (Object) b.longValue() : b;
    if (p instanceof BigDecimal b) {
      BigDecimal s = b.stripTrailingZeros();
      if (s.scale() <= 0 && s.precision() - s.scale() < 19) return s.longValueExact();
      double d = s.doubleValue();
      return Double.isFinite(d) && new BigDecimal(d).compareTo(s) == 0 ? (Object) d : s;
    }
    if (p instanceof float[] f) return floats(f);
    if (p instanceof double[] d) {
      float[] f = new float[d.length];
      for (int i = 0; i < d.length; i++) f[i] = (float) d[i];
      return floats(f);
    }
    if (p instanceof byte[] b) {
      List<Byte> l = new ArrayList<>(b.length);
      for (byte x : b) l.add(x);
      return List.copyOf(l);
    }
    if (p instanceof CharSequence s) return s.toString();
    if (p instanceof MleJson j) return List.of("json", j.text());
    return p;
  }

  private static Object number(double d) {
    return d == Math.rint(d) && Math.abs(d) < 0x1p63 ? (Object) (long) d : (Object) d;
  }

  private static List<Float> floats(float[] f) {
    List<Float> l = new ArrayList<>(f.length);
    for (float x : f) l.add(x);
    return List.copyOf(l);
  }

  // -------------------------------------------------------- for the worker

  /** The session ended (RESET): every set goes. */
  public static void reset() {
    TABLE.clear();
  }

  /** Drops the sets idle for their idle time (when a request comes, and while the worker waits). */
  public static void expire() {
    TABLE.expire();
  }

  /** System.nanoTime() of the next expiry, or Long.MAX_VALUE. */
  public static long nextExpiry() {
    return TABLE.nextExpiry();
  }

  /** Whether drops, expiry or eviction freed enough (64 MiB, or the whole cache) to give memory back. */
  public static boolean giveBackDue() {
    return TABLE.giveBackDue();
  }

  /** The worker gave memory back (it ran a GC): what is freed from now on counts anew. */
  public static void gaveBack() {
    TABLE.gaveBack();
  }
}
