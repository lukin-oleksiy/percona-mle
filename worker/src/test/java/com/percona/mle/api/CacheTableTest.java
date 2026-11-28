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

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

/** The vector cache's table (exp note 76): eviction, shared bases, names, refresh, expiry, threads. */
class CacheTableTest {
  /** A value of given bytes, sharing a base (any object) or not. */
  record Fake(Object base, long baseBytes, long ownBytes) implements CacheTable.Sized {
    public int rows() { return 1; }

    public int dimensions() { return 1; }
  }

  static final long S = 1_000_000_000L;  // a second in nanoseconds
  final AtomicLong now = new AtomicLong(1000 * S);

  CacheTable table(long max, long idleS) {
    return new CacheTable(max, idleS * S, now::get);
  }

  static CacheTable.Key k(String name) {
    return new CacheTable.Key("app", name);
  }

  static Fake own(long bytes) {
    return new Fake(null, 0, bytes);
  }

  /** Whether a give-back is due; then it ran. */
  static boolean take(CacheTable t) {
    boolean due = t.giveBackDue();
    if (due) t.gaveBack();
    return due;
  }

  CacheTable.Sized load(CacheTable t, String name, CacheTable.Sized v) {
    return t.cached(k(name), "vectors", "SELECT " + name, List.of(), new Object[0], () -> v);
  }

  @Test
  void leastRecentlyUsedGoesFirst() {
    CacheTable t = table(1000, 0);
    load(t, "a", own(400));
    load(t, "b", own(400));
    t.kept(k("a"));  // b is now the least recently used
    load(t, "c", own(400));
    assertNull(t.kept(k("b")));
    assertTrue(t.kept(k("a")) != null && t.kept(k("c")) != null);
    assertEquals(800, t.total());
  }

  @Test
  void aSharedBaseCountsOnceAndGoesWithItsLastUser() {
    CacheTable t = table(10_000, 0);
    Object base = new Object();
    t.keep(k("view1"), "vectors", new Fake(base, 6000, 50));
    t.keep(k("view2"), "vectors", new Fake(base, 6000, 70));
    assertEquals(6120, t.total());
    t.drop(k("view1"));
    assertEquals(6070, t.total());  // the base stays for view2
    assertFalse(take(t));
    t.keep(k("other"), "vectors", own(5000));  // needs the base's room: view2 (and its base) evicted
    assertNull(t.kept(k("view2")));
    assertEquals(5000, t.total());
  }

  @Test
  void evictionGoesOnUntilTheBytesReallyFreedAreEnough() {
    CacheTable t = table(10_000, 0);
    Object base = new Object();
    t.keep(k("v1"), "vectors", new Fake(base, 8000, 10));
    t.keep(k("v2"), "vectors", new Fake(base, 8000, 10));
    t.keep(k("small"), "vectors", own(1000));
    // 9020 kept; 3000 more: v1 alone frees 10 bytes, so v2 (and the base) go too, small stays
    t.keep(k("new"), "vectors", own(3000));
    assertNull(t.kept(k("v1")));
    assertNull(t.kept(k("v2")));
    assertTrue(t.kept(k("small")) != null);
    assertEquals(4000, t.total());
  }

  @Test
  void aSetLargerThanTheLimitIsReturnedNotKeptAndEvictsNothing() {
    CacheTable t = table(1000, 0);
    load(t, "a", own(600));
    Fake big = own(2000);
    assertSame(big, load(t, "big", big));
    assertNull(t.kept(k("big")));
    assertTrue(t.kept(k("a")) != null);
    List<CacheTable.Info> info = t.info();
    assertTrue(info.get(info.size() - 1).state().startsWith("not kept"));
  }

  @Test
  void oneNameOneQueryOneKind() {
    CacheTable t = table(10_000, 0);
    t.cached(k("a"), "vectors", "SELECT 1", List.of(5L), new Object[] {5L}, () -> own(10));
    assertThrows(IllegalArgumentException.class,
        () -> t.cached(k("a"), "vectors", "SELECT 2", List.of(5L), new Object[] {5L}, () -> own(10)));
    assertThrows(IllegalArgumentException.class,
        () -> t.cached(k("a"), "vectors", "SELECT 1", List.of(6L), new Object[] {6L}, () -> own(10)));
    assertThrows(IllegalArgumentException.class,
        () -> t.cached(k("a"), "int8", "SELECT 1", List.of(5L), new Object[] {5L}, () -> own(10)));
    // the same name in another schema is another set
    t.cached(new CacheTable.Key("other", "a"), "vectors", "SELECT 2", List.of(), new Object[0], () -> own(10));
    t.keep(k("k"), "vectors", own(10));
    assertThrows(IllegalArgumentException.class, () -> load(t, "k", own(10)));
    assertThrows(IllegalArgumentException.class, () -> t.refresh(k("k"), e -> own(10)));
    Fake replaced = own(20);
    assertTrue(t.keep(k("k"), "vectors", replaced));  // keep replaces
    assertSame(replaced, t.kept(k("k")));
  }

  @Test
  void refreshReplacesKeepsTheExpiryAndKeepsTheOldSetOnFailure() {
    CacheTable t = table(10_000, 60);
    Fake first = own(10);
    load(t, "a", first);
    t.expireAfter(k("a"), 5000);
    Fake second = own(20);
    assertSame(second, t.refresh(k("a"), e -> second));
    assertEquals(5000, t.info().get(0).idleMs());
    assertThrows(IllegalStateException.class, () -> t.refresh(k("a"), e -> {
      throw new IllegalStateException("the table is gone");
    }));
    assertSame(second, t.kept(k("a")));
    assertNull(t.refresh(k("never"), e -> own(1)));  // a name not kept: null, nothing done
  }

  @Test
  void aFailedLoadKeepsNothingAndTheNextCallLoads() {
    CacheTable t = table(10_000, 0);
    assertThrows(IllegalStateException.class, () -> t.cached(k("a"), "vectors", "SELECT 1", List.of(), new Object[0],
        () -> { throw new IllegalStateException("killed"); }));
    assertEquals(0, t.size());
    Fake v = own(10);
    assertSame(v, load(t, "a", v));
  }

  @Test
  void aLoadThatAsksForItsOwnNameFailsInsteadOfWaiting() {
    CacheTable t = table(10_000, 0);
    assertThrows(IllegalStateException.class, () -> t.cached(k("a"), "vectors", "SELECT 1", List.of(), new Object[0],
        () -> load(t, "a", own(1))));
    assertEquals(0, t.size());
    assertSame(t.cached(k("a"), "vectors", "SELECT 1", List.of(), new Object[0], () -> own(2)), t.kept(k("a")));
  }

  @Test
  void idleSetsExpireAtTheirDeadline() {
    CacheTable t = table(10_000, 60);
    load(t, "a", own(10));
    load(t, "b", own(10));
    t.expireAfter(k("b"), 0);  // never
    assertEquals(now.get() + 60 * S, t.nextExpiry());
    now.addAndGet(59 * S);
    t.kept(k("a"));  // used: its idle time starts again
    now.addAndGet(59 * S);
    t.expire();
    assertTrue(t.kept(k("a")) != null);
    now.addAndGet(61 * S);
    t.expire();
    assertNull(t.kept(k("a")));
    assertTrue(t.kept(k("b")) != null);
    assertEquals(Long.MAX_VALUE, t.nextExpiry());
    assertFalse(t.expireAfter(k("gone"), 10));
    assertThrows(IllegalArgumentException.class, () -> t.expireAfter(k("b"), -1));
  }

  @Test
  void aLargeDisposalOrTheWholeCacheOf8MiBAsksForAGiveBack() {
    CacheTable t = table(1L << 32, 0);
    load(t, "big", own(100L << 20));
    load(t, "small", own(1 << 20));
    t.drop(k("small"));
    assertFalse(take(t));  // 1 MiB: not worth a GC
    t.drop(k("big"));
    assertTrue(take(t));  // 100 MiB, and the cache is empty
    assertFalse(take(t));  // once
    load(t, "x", own(10));
    t.dropAll();
    assertFalse(take(t));  // the whole cache, but 10 bytes: not worth a GC (T3)
    load(t, "x", own(9L << 20));
    t.dropAll();
    assertTrue(take(t));  // the whole cache and 9 MiB freed since the last give-back
    load(t, "y", own(10));
    t.clear();  // RESET: its own GC, no give-back
    assertFalse(take(t));
  }

  @Test
  void anOffCacheLoadsEveryTimeAndKeepsNothing() {
    CacheTable t = table(0, 0);
    AtomicInteger loads = new AtomicInteger();
    for (int i = 0; i < 3; i++)
      t.cached(k("a"), "vectors", "SELECT 1", List.of(), new Object[0], () -> {
        loads.incrementAndGet();
        return own(1);
      });
    assertEquals(3, loads.get());
    assertFalse(t.keep(k("b"), "vectors", own(1)));
    assertNull(t.kept(k("a")));
    assertNull(t.refresh(k("a"), e -> own(1)));
  }

  @Test
  void threadsAskingForOneNameLoadItOnce() throws Exception {
    CacheTable t = table(10_000, 0);
    AtomicInteger loads = new AtomicInteger();
    CountDownLatch start = new CountDownLatch(1);
    List<Thread> threads = new ArrayList<>();
    List<Object> got = java.util.Collections.synchronizedList(new ArrayList<>());
    for (int i = 0; i < 8; i++) {
      Thread th = new Thread(() -> {
        try {
          start.await();
          got.add(t.cached(k("a"), "vectors", "SELECT 1", List.of(), new Object[0], () -> {
            loads.incrementAndGet();
            try {
              Thread.sleep(50);
            } catch (InterruptedException e) {
              Thread.currentThread().interrupt();
            }
            return own(10);
          }));
        } catch (InterruptedException e) {
          Thread.currentThread().interrupt();
        }
      });
      th.start();
      threads.add(th);
    }
    start.countDown();
    for (Thread th : threads) th.join();
    assertEquals(1, loads.get());
    assertEquals(8, got.size());
    for (Object o : got) assertSame(got.get(0), o);
  }

  @Test
  void manyThreadsKeepTheTableConsistent() throws Exception {
    CacheTable t = table(5_000, 0);
    Object base = new Object();
    List<Thread> threads = new ArrayList<>();
    AtomicInteger failures = new AtomicInteger();
    for (int i = 0; i < 8; i++) {
      final int seed = i;
      Thread th = new Thread(() -> {
        java.util.Random r = new java.util.Random(seed);
        for (int n = 0; n < 20_000; n++) {
          try {
            String name = "s" + r.nextInt(20);
            switch (r.nextInt(5)) {
              case 0 -> t.keep(k(name), "vectors", r.nextBoolean() ? own(r.nextInt(900)) : new Fake(base, 1500, 10));
              case 1 -> t.kept(k(name));
              case 2 -> t.drop(k(name));
              case 3 -> t.info();
              default -> t.expire();
            }
          } catch (RuntimeException e) {
            failures.incrementAndGet();
          }
        }
      });
      th.start();
      threads.add(th);
    }
    for (Thread th : threads) th.join();
    assertEquals(0, failures.get());
    assertTrue(t.total() <= 5_000);
  }

  @Test
  void aThreadThatMayNotLoadWaitsForALoadOrFails() throws Exception {
    CacheTable t = table(1000, 0);
    assertThrows(IllegalStateException.class,
        () -> t.cached(k("a"), "vectors", "SELECT a", List.of(), new Object[0], () -> own(10), false));
    assertNull(t.kept(k("a")));
    CountDownLatch loading = new CountDownLatch(1), release = new CountDownLatch(1);
    Fake v = own(10);
    Thread loader = new Thread(() -> t.cached(k("a"), "vectors", "SELECT a", List.of(), new Object[0], () -> {
      loading.countDown();
      try {
        release.await();
      } catch (InterruptedException e) {
        throw new IllegalStateException(e);
      }
      return v;
    }));
    loader.start();
    loading.await();
    AtomicReference<Object> got = new AtomicReference<>();
    Thread waiter = new Thread(() -> got.set(
        t.cached(k("a"), "vectors", "SELECT a", List.of(), new Object[0], () -> own(99), false)));
    waiter.start();
    Thread.sleep(50);
    assertTrue(waiter.isAlive());  // waits for the load under way
    release.countDown();
    loader.join();
    waiter.join();
    assertSame(v, got.get());
  }

  @Test
  void waitersAreReleasedWhenKeepingTheLoadedSetFails() throws Exception {
    CacheTable t = table(1000, 0);
    CountDownLatch loading = new CountDownLatch(1), release = new CountDownLatch(1);
    // A value whose bytes cannot be counted (as an OutOfMemoryError in put() would).
    CacheTable.Sized broken = new CacheTable.Sized() {
      public Object base() { return null; }
      public long baseBytes() { return 0; }
      public long ownBytes() { throw new OutOfMemoryError("test"); }
      public int rows() { return 1; }
      public int dimensions() { return 1; }
    };
    Thread loader = new Thread(() -> {
      try {
        t.cached(k("a"), "vectors", "SELECT a", List.of(), new Object[0], () -> {
          loading.countDown();
          try {
            release.await();
          } catch (InterruptedException e) {
            throw new IllegalStateException(e);
          }
          return broken;
        });
      } catch (OutOfMemoryError expected) {
        // the loading call fails
      }
    });
    loader.start();
    loading.await();
    AtomicReference<Object> got = new AtomicReference<>();
    Thread waiter = new Thread(() -> got.set(load(t, "a", own(5))));
    waiter.start();
    Thread.sleep(50);
    release.countDown();
    waiter.join(5000);
    assertFalse(waiter.isAlive(), "a waiter of a failed put() must not hang");
    assertEquals(5, ((Fake) got.get()).ownBytes());  // it loaded the name itself
  }

  @Test
  void aGiveBackStaysDueUntilItRuns() {
    CacheTable t = table(1L << 40, 0);
    load(t, "a", own(100L << 20));
    t.drop(k("a"));
    assertTrue(t.giveBackDue());
    assertTrue(t.giveBackDue());  // looking does not take it
    t.gaveBack();
    assertFalse(t.giveBackDue());
  }

  @Test
  void indexesBuiltAfterKeepingAreEvictedAtTheNextExpiry() {
    CacheTable t = table(1000, 0);
    AtomicLong grows = new AtomicLong(400);
    CacheTable.Sized growing = new CacheTable.Sized() {
      public Object base() { return null; }
      public long baseBytes() { return 0; }
      public long ownBytes() { return grows.get(); }
      public int rows() { return 1; }
      public int dimensions() { return 1; }
    };
    load(t, "a", growing);
    load(t, "b", own(500));
    grows.set(700);  // an index built after keep: 1200 > 1000
    t.expire();
    assertNull(t.kept(k("a")));
    assertEquals(500, t.total());
    t.expire();  // the last set stays even when its own index takes it over
    grows.set(2000);
    load(t, "a2", growing);  // refused (over the limit alone)
    assertNull(t.kept(k("a2")));
  }

  @Test
  void aFarExpiryDoesNotOverflow() {
    CacheTable t = table(1000, 0);
    load(t, "a", own(10));
    load(t, "b", own(10));
    t.expireAfter(k("a"), Long.MAX_VALUE / 1_000_000L);
    t.expireAfter(k("b"), 5_000);
    assertEquals(now.get() + 5 * S, t.nextExpiry());
    t.drop(k("b"));
    long next = t.nextExpiry();
    assertTrue(next > now.get() && next <= now.get() + CacheTable.FAR_NS);
  }

  @Test
  void aValueKeptUnderTwoNamesCountsOnce() {
    CacheTable t = table(1000, 0);
    Fake v = own(600);
    t.keep(k("a"), "vectors", v);
    assertTrue(t.keep(k("b"), "vectors", v));  // fits: the same 600 bytes
    assertEquals(600, t.total());
    assertSame(v, t.kept(k("a")));
  }

  @Test
  void aSetTheJvmClearedIsForgottenAndLoadedAgain() {
    CacheTable t = table(1000, 0);
    Fake first = own(300), second = own(300);
    load(t, "a", first);
    load(t, "b", own(200));
    t.clearReference(k("a"));  // as the JVM before an OutOfMemoryError
    assertEquals(200, t.total());  // its bytes no longer count
    assertSame(second, load(t, "a", second));  // loaded again
    assertSame(second, t.kept(k("a")));
    t.clearReference(k("b"));
    assertNull(t.kept(k("b")));
    List<CacheTable.Info> info = t.info();
    CacheTable.Info last = info.get(info.size() - 1);
    assertEquals("b", last.key().name());
    assertTrue(last.state().startsWith("forgotten"));
    assertFalse(take(t));  // no give-back: the GC that cleared it has the memory
    t.clearReference(k("a"));
    assertFalse(t.drop(k("a")));  // already forgotten
    assertEquals(0, t.size());
  }
}
