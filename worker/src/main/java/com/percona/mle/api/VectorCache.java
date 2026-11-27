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

  /** The limit: mle.vector_cache_max (-Dmle.vectorCacheMax), -1 (the default) for a quarter of the heap. */
  static final long MAX = limit(Long.getLong("mle.vectorCacheMax", -1));
  /** The default idle time: mle.vector_cache_idle_ms (-Dmle.vectorCacheIdleMs), 30 minutes; 0: never. */
  static final long IDLE_MS = Math.max(0, Long.getLong("mle.vectorCacheIdleMs", 1_800_000L));

  static final CacheTable TABLE = new CacheTable(MAX, IDLE_MS * 1_000_000L, System::nanoTime);

  static long limit(long setting) {
    return setting < 0 ? Runtime.getRuntime().maxMemory() / 4 : setting;
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
    return (VectorSet) TABLE.cached(key(name), "vectors", sql, values(p), p, () -> VectorSet.load(sql, p));
  }

  static QuantizedSet cachedQuantized(String name, String type, String sql, Object[] params) {
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    String kind = QuantizedSet.binaryType(type) ? "binary" : "int8";
    Object[] p = javaValues(params);
    return (QuantizedSet) TABLE.cached(key(name), kind, sql, values(p), p, () -> QuantizedSet.load(kind, sql, p));
  }

  static Object refresh(String name) {
    return TABLE.refresh(key(name), e -> {
      Object[] p = e.raw;
      return e.kind.equals("vectors") ? VectorSet.load(e.sql, p) : QuantizedSet.load(e.kind, e.sql, p);
    });
  }

  /**
   * The parameters as Java values, taken once (a JavaScript array changed after the call does not
   * change the kept query): a guest number as Long or Double, text as String, an array or numpy
   * array as float[] (a VECTOR), a host object as itself.
   */
  static Object[] javaValues(Object[] params) {
    if (params == null) return new Object[0];
    Object[] out = new Object[params.length];
    for (int i = 0; i < out.length; i++) {
      Object p = params[i];
      if (p instanceof Value v) {
        if (v.isNull()) p = null;
        else if (v.isHostObject()) p = v.asHostObject();
        else if (v.isBoolean()) p = v.asBoolean();
        else if (v.isNumber()) p = v.fitsInLong() ? (Object) v.asLong() : (Object) v.asDouble();
        else if (v.isString()) p = v.asString();
        else if (v.hasArrayElements() || v.hasMember("tobytes")) p = GuestVectors.vector(v, "parameter " + i);
      }
      out[i] = p;
    }
    return out;
  }

  /**
   * The parameters as the values they become in SQL, to compare by value: integers as Long (or
   * BigInteger), other numbers as Double or a BigDecimal without trailing zeros, text as String, a
   * VECTOR or bytes as a list of their elements; JavaScript and Python values the same way.
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
    if (p instanceof Value v) {
      if (v.isNull()) return null;
      if (v.isHostObject()) return value(v.asHostObject());
      if (v.isBoolean()) return v.asBoolean() ? 1L : 0L;
      if (v.isNumber()) return v.fitsInLong() ? (Object) v.asLong() : (Object) v.asDouble();
      if (v.isString()) return v.asString();
      if (v.hasArrayElements() || v.hasMember("tobytes")) return floats(GuestVectors.vector(v, "a parameter"));
      return v.toString();
    }
    if (p instanceof Long || p instanceof Integer || p instanceof Short || p instanceof Byte) return ((Number) p).longValue();
    if (p instanceof Boolean b) return b ? 1L : 0L;
    if (p instanceof Double || p instanceof Float) {
      double d = ((Number) p).doubleValue();
      return d == Math.rint(d) && Math.abs(d) < 0x1p63 ? (Object) (long) d : (Object) d;
    }
    if (p instanceof BigInteger b) return b.bitLength() < 64 ? (Object) b.longValue() : b;
    if (p instanceof BigDecimal b) {
      BigDecimal s = b.stripTrailingZeros();
      return s.scale() <= 0 && s.precision() - s.scale() < 19 ? (Object) s.longValueExact() : s;
    }
    if (p instanceof float[] f) return floats(f);
    if (p instanceof byte[] b) {
      List<Byte> l = new ArrayList<>(b.length);
      for (byte x : b) l.add(x);
      return List.copyOf(l);
    }
    if (p instanceof CharSequence s) return s.toString();
    return value(Value.asValue(p));
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

  /** Whether drops, expiry or eviction freed enough (64 MiB, or the whole cache) to give memory back; resets it. */
  public static boolean takeGiveBack() {
    return TABLE.takeGiveBack();
  }
}
