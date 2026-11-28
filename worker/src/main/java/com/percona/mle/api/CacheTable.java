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
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/**
 * The table of a worker's vector cache (exp note 76): entries by (schema, name), their bytes, least
 * recently used first, idle expiry. Not API; {@link VectorCache} adapts it to the library and the
 * worker. Knows nothing of SQL: a load is a {@code Supplier}, time a {@code LongSupplier} of
 * nanoseconds (the unit tests drive both).
 *
 * <p>Thread-safe: the table is guarded by this object's lock; a load runs outside the lock, and
 * threads that ask for a name while it is loaded wait for that load (one load per name). Only a
 * thread that may load (the routine's own: SQL runs there only) starts a load; another thread
 * waits for a load under way or gets an error. Bytes are
 * counted per base: a view, or a quantized set that rescores with a set's floats, shares that set's
 * vectors, which count once however many entries use them; removing an entry frees its base only
 * when no other entry uses it; a value kept under two names counts once.
 */
final class CacheTable {
  /** What an entry holds: a VectorSet or a QuantizedSet. */
  interface Sized {
    /** The vectors this value shares with others (a view's root set), or null. */
    Object base();

    /** The bytes of the base (counted once for all entries that share it). */
    long baseBytes();

    /** The bytes of this value apart from its base. */
    long ownBytes();

    int rows();

    int dimensions();
  }

  record Key(String schema, String name) {}

  /** One kept set. Fields other than the final ones change under the table's lock. */
  static final class Entry {
    final Key key;
    final String kind;  // "vectors", "int8", "binary"
    final String sql;  // null: kept by keep()
    final List<Object> params;  // as compared (by value)
    final Object[] raw;  // as passed to the query (Java values)
    Sized value;
    final long loadedAtMs;
    long lastUsedNs, uses, idleNs;

    Entry(Key key, String kind, String sql, List<Object> params, Object[] raw, Sized value, long now, long idleNs) {
      this.key = key;
      this.kind = kind;
      this.sql = sql;
      this.params = params;
      this.raw = raw;
      this.value = value;
      this.loadedAtMs = System.currentTimeMillis();
      this.lastUsedNs = now;
      this.idleNs = idleNs;
    }
  }

  /** What info() returns for one entry, or for the last set that was not kept (value null). */
  record Info(Key key, String kind, int rows, int dimensions, long bytes, long loadedAtMs, long idleMs, long expiresInMs,
      long uses, String sql, String state) {}

  static final long GIVE_BACK = 64L << 20;

  private final long max;
  private final long defaultIdleNs;
  private final LongSupplier clock;
  /** Access order: the first is the least recently used. */
  private final LinkedHashMap<Key, Entry> entries = new LinkedHashMap<>(16, 0.75f, true);
  private final Map<Key, CompletableFuture<Void>> loading = new java.util.HashMap<>();
  /** The thread loading each name: a load query that asks for its own name (a function in it) fails, not waits. */
  private final Map<Key, Thread> loaders = new java.util.HashMap<>();
  private Info refused;
  private long freed;
  private boolean giveBackDue;

  CacheTable(long max, long defaultIdleNs, LongSupplier clock) {
    this.max = max;
    this.defaultIdleNs = defaultIdleNs;
    this.clock = clock;
  }

  boolean enabled() {
    return max > 0;
  }

  long max() {
    return max;
  }

  /**
   * The set kept under key, or loaded with loader and kept. A name kept under another query, kind
   * or by keep() is an error. With the cache off: loaded every time, not kept. mayLoad: whether this
   * thread may run the loader; if not, it waits for a load under way, or fails when there is none
   * (rather than a load that fails or succeeds by which thread came first).
   */
  Sized cached(Key key, String kind, String sql, List<Object> params, Object[] raw, Supplier<Sized> loader,
      boolean mayLoad) {
    if (!enabled()) return loader.get();
    while (true) {
      CompletableFuture<Void> wait;
      CompletableFuture<Void> mine = null;
      synchronized (this) {
        Entry e = entries.get(key);
        if (e != null) {
          same(e, kind, sql, params);
          touch(e);
          return e.value;
        }
        wait = loading.get(key);
        if (wait == null && !mayLoad) {
          throw new IllegalStateException("'" + key.name() + "' is not kept and cannot be loaded on this thread"
              + " (SQL runs only on the thread that runs the routine): ask for it there first");
        }
        if (wait == null) {
          loading.put(key, mine = new CompletableFuture<>());
          loaders.put(key, Thread.currentThread());
        } else if (loaders.get(key) == Thread.currentThread()) {
          throw new IllegalStateException("'" + key.name() + "' is being loaded by this call: its query cannot use it");
        }
      }
      if (mine == null) {  // another thread loads this name: wait, then look again (its load may have failed)
        wait.join();
        continue;
      }
      // The waiters are released whatever happens, an OutOfMemoryError in put() too: complete(null)
      // allocates nothing, and a waiter looks again either way.
      try {
        Sized value = loader.get();
        Entry n = new Entry(key, kind, sql, params, raw, value, clock.getAsLong(), defaultIdleNs);
        synchronized (this) {
          loading.remove(key);
          loaders.remove(key);
          put(n, null);
        }
        return value;
      } finally {
        synchronized (this) {
          loading.remove(key, mine);
          loaders.remove(key);
        }
        mine.complete(null);
      }
    }
  }

  /** cached(...) by a thread that may load. */
  Sized cached(Key key, String kind, String sql, List<Object> params, Object[] raw, Supplier<Sized> loader) {
    return cached(key, kind, sql, params, raw, loader, true);
  }

  private static void same(Entry e, String kind, String sql, List<Object> params) {
    if (e.sql == null)
      throw new IllegalArgumentException("'" + e.key.name() + "' is kept by keep(), without a query: use kept()");
    if (!e.kind.equals(kind))
      throw new IllegalArgumentException("'" + e.key.name() + "' is kept as " + e.kind + ", not " + kind
          + ": one name, one kind");
    if (!e.sql.equals(sql) || !Objects.equals(e.params, params))
      throw new IllegalArgumentException("'" + e.key.name() + "' is kept for another query (" + e.sql
          + (e.params.isEmpty() ? "" : " with " + e.params) + "): one name, one query; drop it first");
  }

  /** Keeps value under key, replacing what was there (the default expiry again); false if not kept. */
  synchronized boolean keep(Key key, String kind, Sized value) {
    if (!enabled()) return false;
    return put(new Entry(key, kind, null, List.of(), new Object[0], value, clock.getAsLong(), defaultIdleNs), null);
  }

  /** The value kept under key, or null. */
  synchronized Sized kept(Key key) {
    Entry e = entries.get(key);
    if (e == null) return null;
    touch(e);
    return e.value;
  }

  /**
   * Loads the entry's query again (reloader: the entry's kind, query and parameters to a value) and
   * replaces its value, keeping its expiry. Null when the name is not kept; an error for an entry
   * kept by keep(). A failed load keeps the old value.
   */
  Sized refresh(Key key, java.util.function.Function<Entry, Sized> reloader) {
    Entry e;
    synchronized (this) {
      e = entries.get(key);
      if (e == null) return null;
      if (e.sql == null)
        throw new IllegalArgumentException("'" + key.name() + "' is kept by keep(), without a query to run again");
    }
    Sized value = reloader.apply(e);  // a failure leaves the entry as it was
    synchronized (this) {
      if (entries.get(key) == e) {  // still the same entry: replace its value, keep its expiry
        Entry n = new Entry(key, e.kind, e.sql, e.params, e.raw, value, clock.getAsLong(), e.idleNs);
        n.uses = e.uses + 1;
        put(n, e);
      }
    }
    return value;
  }

  synchronized boolean drop(Key key) {
    long before = total();
    boolean had = entries.remove(key) != null;
    if (had) freed(before);
    return had;
  }

  synchronized void dropAll() {
    if (entries.isEmpty()) return;
    long before = total();
    entries.clear();
    freed(before);
  }

  /** The end of the session (RESET): everything goes, without a give-back of its own (RESET's GC does it). */
  synchronized void clear() {
    entries.clear();
    refused = null;
    freed = 0;
    giveBackDue = false;
  }

  /** Sets an entry's idle time (0: never expires); false if the name is not kept. */
  synchronized boolean expireAfter(Key key, long idleMs) {
    if (idleMs < 0) throw new IllegalArgumentException("idleMs must be at least 0 (0: never), not " + idleMs);
    Entry e = entries.get(key);
    if (e == null) return false;
    e.idleNs = idleMs > Long.MAX_VALUE / 1_000_000L ? 0 : idleMs * 1_000_000L;
    return true;
  }

  /**
   * Drops the entries idle for their idle time by now; then, if indexes built since the sets were
   * kept (cosine norms, ids, labels) took the bytes over the limit, the least recently used until
   * they fit (put() is the only other place that evicts).
   */
  synchronized void expire() {
    long now = clock.getAsLong();
    long before = total();
    boolean any = false;
    for (Iterator<Entry> it = entries.values().iterator(); it.hasNext(); ) {
      Entry e = it.next();
      if (e.idleNs > 0 && now - e.lastUsedNs >= e.idleNs) {
        it.remove();
        any = true;
      }
    }
    for (Iterator<Entry> it = entries.values().iterator(); it.hasNext() && entries.size() > 1 && total() > max; ) {
      it.next();
      it.remove();
      any = true;
    }
    if (any) freed(before);
  }

  /** Expiries further than this are looked at again then (no overflow of the clock's time). */
  static final long FAR_NS = 86_400_000_000_000L;

  /** The clock's time of the next expiry (at most FAR_NS away), or Long.MAX_VALUE. */
  synchronized long nextExpiry() {
    long now = clock.getAsLong();
    long next = Long.MAX_VALUE;  // nanoseconds from now
    for (Entry e : entries.values())
      if (e.idleNs > 0) next = Math.min(next, Math.max(0, e.idleNs - (now - e.lastUsedNs)));
    return next == Long.MAX_VALUE ? next : now + Math.min(next, FAR_NS);
  }

  /** Whether disposals freed enough since the last give-back to give memory back. */
  synchronized boolean giveBackDue() {
    return giveBackDue;
  }

  /** The memory was given back (a GC ran): count anew. */
  synchronized void gaveBack() {
    giveBackDue = false;
    freed = 0;
  }

  /** The bytes kept: every value's own bytes once (a value may be kept under two names), every base once. */
  synchronized long total() {
    long t = 0;
    IdentityHashMap<Object, Boolean> values = new IdentityHashMap<>(), bases = new IdentityHashMap<>();
    for (Entry e : entries.values()) {
      if (values.put(e.value, Boolean.TRUE) == null) t += e.value.ownBytes();
      Object b = e.value.base();
      if (b != null && bases.put(b, Boolean.TRUE) == null) t += e.value.baseBytes();
    }
    return t;
  }

  synchronized int size() {
    return entries.size();
  }

  /** The entries, least recently used first, then the last set that was not kept (if any). */
  synchronized List<Info> info() {
    long now = clock.getAsLong();
    List<Info> out = new ArrayList<>();
    for (Entry e : entries.values()) {
      long bytes = e.value.ownBytes() + (e.value.base() != null ? e.value.baseBytes() : 0);
      long expires = e.idleNs == 0 ? -1 : Math.max(0, (e.lastUsedNs + e.idleNs - now) / 1_000_000L);
      out.add(new Info(e.key, e.kind, e.value.rows(), e.value.dimensions(), bytes, e.loadedAtMs, e.idleNs / 1_000_000L,
          expires, e.uses, e.sql, "kept"));
    }
    if (refused != null) out.add(refused);
    return out;
  }

  private void touch(Entry e) {
    e.lastUsedNs = clock.getAsLong();
    e.uses++;
  }

  /**
   * Puts n (replacing old, or what is under its key), then evicts the least recently used others
   * until the bytes fit. A value larger than the limit alone is not kept and evicts nothing.
   */
  private boolean put(Entry n, Entry old) {
    Entry previous = old != null ? old : entries.get(n.key);
    // The bytes n adds: its own and its base, each unless another entry (not the one it replaces) has it.
    Object base = n.value.base();
    boolean sameValue = false, sharedBase = false;
    for (Entry e : entries.values()) {
      if (e == previous) continue;
      if (e.value == n.value) sameValue = true;
      if (base != null && e.value.base() == base) sharedBase = true;
    }
    long adds = (sameValue ? 0 : n.value.ownBytes()) + (base != null && !sharedBase ? n.value.baseBytes() : 0);
    if (adds > max) {
      refused = new Info(n.key, n.kind, n.value.rows(), n.value.dimensions(), adds, n.loadedAtMs, 0, -1, 0, n.sql,
          "not kept: " + mib(adds) + " is more than mle.vector_cache_max (" + mib(max) + ")");
      if (previous != null) {  // a replacement that does not fit: the old one goes too (it is stale)
        long before = total();
        entries.remove(n.key);
        freed(before);
      }
      return false;
    }
    long before = total();
    entries.put(n.key, n);  // most recently used
    for (Iterator<Entry> it = entries.values().iterator(); it.hasNext() && total() > max; ) {
      Entry e = it.next();
      if (e != n) it.remove();
    }
    long after = total();
    if (after < before + adds) freed(before + adds);
    return true;
  }

  /** Records what a removal freed (before: the total before it, against the total now). */
  private void freed(long before) {
    long f = before - total();
    if (f <= 0) return;
    freed += f;
    if (freed >= GIVE_BACK || entries.isEmpty()) giveBackDue = true;
  }

  static String mib(long bytes) {
    return String.format(java.util.Locale.ROOT, "%.1f MiB", bytes / 1048576.0);
  }
}
