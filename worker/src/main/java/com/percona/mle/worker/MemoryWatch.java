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

package com.percona.mle.worker;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;

/**
 * Native memory that only a Java GC gives back (exp notes 30, 45). GraalPy frees a native object
 * (a numpy array) when the Java object referring to it is collected; numpy allocates with malloc,
 * which does not make the JVM collect, so a loop of large temporaries grows until the worker's
 * cgroup limit kills it. GraalPy's own remedies (its "Python GC" task, MaxNativeMemory) end in
 * System.gc() and do not see numpy's malloc.
 *
 * <p>While a top-level call runs, a thread reads the process's resident size every 10 ms; when it
 * has grown by a twentieth of the worker's memory limit (mle.memoryMax, from mle.memory_max) since
 * the last young GC it caused (or since the lowest size seen), it causes a young GC: it allocates
 * short-lived garbage until a weak reference to a fresh object is cleared. The JVM collects as it
 * always does when the young generation is full (~1 ms), which frees the numpy arrays that died
 * young. No System.gc() on the way: a forced full GC took 125-190 ms (250-800 ms under load) and
 * made one call in ~25 of a 16 MB fetch_columns stream that slow (exp note 45: p99 138 -> 22 ms).
 * Only as a last resort, when the size is still above 85 % of the limit after a young GC and has
 * grown by a twentieth of the limit since the last full GC (or the lowest size seen), it forces a
 * full GC: objects that reached the old generation hold their native memory until one runs, and
 * the JVM starts one only when its heap is full (a worker running many small numpy arrays was
 * killed without it, exp note 46). A call that holds much data steadily is not collected again
 * and again; the P2-P4 benchmarks stay at about half of the limit and never get there. Freed
 * arrays reach the system at once because workers run with a fixed malloc mmap threshold
 * (mle.malloc_mmap_threshold), and the launcher sets the cgroup's memory.high to 90 % of
 * memory.max, so that the kernel throttles an allocating thread instead of killing the worker.
 * Idle workers are left alone: RESET and the pool already trim them. The count goes to the host
 * with the worker's statistics (mle_worker_memory_gcs) and restarts at RESET. A check allocates
 * nothing: /proc/self/status is re-read through a file kept open, into a reused buffer.
 *
 * <p>The thread also writes the worker's statistics (§3.1), so that the thread that serves
 * requests writes none on a request's path: at each of its periods while calls run, when the
 * figures have changed enough to matter (the state, the count of forced GCs, or the heap in use by
 * 1/64 of its limit), and once when the worker has been idle for a period.
 */
final class MemoryWatch implements Runnable {
  /** 10 ms: a numpy loop can allocate several GB/s (reading the size costs microseconds). */
  private static final long PERIOD_MS = Long.getLong("mle.memoryWatchMs", 10);
  private static final byte[] VM_RSS = "VmRSS:".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
  private static final AtomicLong GCS = new AtomicLong();
  private static volatile MemoryWatch instance;

  private final long limit;
  private final Thread thread;
  private final Mailbox mailbox;
  /** What the serving thread is doing (Wire.STATE_RUNNING or STATE_COMPILING): read here without
      synchronization, for the statistics only. */
  private static int phase = Wire.STATE_RUNNING;
  /** The statistics written last. */
  private int writtenState = -1;
  private long writtenHeap;
  private long writtenGcs = -1;
  /**
   * Calls begun plus calls ended (the main thread writes it): odd while a call runs. One volatile
   * write per begin and per end; the thread parks only after a whole period in which no call
   * began, so a stream of short calls never has to wake it (exp note 37: waking it on every call
   * cost 0.23 µs per call in the native build).
   */
  private volatile long state;
  /** Set before the thread parks; begin() unparks it only then. */
  private volatile boolean parked;
  /** /proc/self/status, kept open, and the buffer it is read into (this thread only, after start). */
  private static RandomAccessFile status;
  private static final byte[] BUFFER = new byte[8192];
  /** The resident size after the last young GC caused here, or the lowest seen since (bytes). */
  private long base = Long.MAX_VALUE;
  /** The resident size after the last full GC forced here, or the lowest seen since (bytes). */
  private long fullBase = Long.MAX_VALUE;
  /** Where the garbage of nudge() goes (a volatile store: it cannot be optimised away). */
  private static volatile Object sink;

  private MemoryWatch(long limit, Mailbox mailbox) {
    this.limit = limit;
    this.mailbox = mailbox;
    this.thread = new Thread(this, "mle-memory");
    thread.setDaemon(true);
    thread.setUncaughtExceptionHandler((t, e) -> Console.workerFailure(t.getName(), e));
  }

  /**
   * Starts the thread: it writes the statistics into mailbox, and watches the memory if the
   * worker has a memory limit and can read its resident size.
   */
  static void start(Mailbox mailbox) {
    long limit = Long.getLong("mle.memoryMax", 0);
    MemoryWatch w = new MemoryWatch(limit <= 0 || rss() < 0 ? 0 : limit, mailbox);
    w.thread.start();
    instance = w;
  }

  /** What the serving thread does from now on, for the statistics (no synchronization). */
  static void phase(int state) {
    phase = state;
  }

  /** The statistics, if they changed enough to matter (state: what the worker is doing now). */
  private void statistics(int state) {
    Runtime rt = Runtime.getRuntime();
    long max = rt.maxMemory();
    long used = rt.totalMemory() - rt.freeMemory();
    long gcs = GCS.get();
    if (state == writtenState && gcs == writtenGcs && Math.abs(used - writtenHeap) < Math.max(max >> 6, 1 << 20))
      return;
    writtenState = state;
    writtenGcs = gcs;
    writtenHeap = used;
    mailbox.stats(state, used, max);
  }

  /** A top-level call starts (the main thread). */
  static void begin() {
    MemoryWatch w = instance;
    if (w == null) return;
    w.state = w.state + 1;  // odd: running (only the main thread writes state)
    // The thread sets parked and then re-reads state: one of the two sees the other's write.
    if (w.parked) LockSupport.unpark(w.thread);
  }

  /** The top-level call ended. */
  static void end() {
    MemoryWatch w = instance;
    if (w != null) w.state = w.state + 1;  // even: idle
  }

  /** GCs forced since the last RESET. */
  static long gcs() {
    return GCS.get();
  }

  /** A GC the worker forced outside a call (the idle trim): counted as the nudges are. */
  static void forcedGc() {
    GCS.incrementAndGet();
  }

  /** RESET: the host has taken the count with the session's statistics. */
  static void clear() {
    GCS.set(0);
  }

  @Override
  public void run() {
    long seen = -1;
    while (true) {
      long s = state;
      if (s == seen && (s & 1) == 0) {  // idle, and no call began during the last period
        statistics(Wire.STATE_IDLE);
        parked = true;
        if (state == s) LockSupport.park(this);
        parked = false;
        seen = -1;
        continue;
      }
      seen = s;
      try {
        Thread.sleep(PERIOD_MS);
      } catch (InterruptedException e) {
        Thread.interrupted();  // nothing interrupts this thread on purpose; go on
      }
      // A failure here (an OutOfMemoryError of a nudge while a routine fills the heap) must not
      // end the watcher: it is logged, and the next period tries again (MVP review JVMP-2).
      try {
        if ((state & 1) != 0) {
          if (limit > 0) check();
          statistics(phase);
        }
      } catch (Throwable t) {
        Console.workerFailure("mle-memory", t);
      }
    }
  }

  private void check() {
    long rss = rss();
    if (rss < 0) return;
    if (rss < base) base = rss;
    if (rss < fullBase) fullBase = rss;
    if (rss - base <= limit / 20) return;
    // Counted first: the call's statistics (sent with its RESULT) must include it.
    GCS.incrementAndGet();
    nudge();
    long after = rss();
    if (after < 0) after = rss;
    base = after;
    if (after > limit / 100 * 85 && after - fullBase > limit / 20) {  // the last resort (see above)
      GCS.incrementAndGet();
      System.gc();
      long full = rss();
      base = fullBase = full < 0 ? after : full;
    }
  }

  /**
   * Causes a young GC: allocates garbage until a weak reference to a fresh object is cleared, at
   * most a quarter of the heap (a GC comes long before; the bound only guards against an
   * unexpected collector).
   */
  private static void nudge() {
    java.lang.ref.WeakReference<Object> probe = new java.lang.ref.WeakReference<>(new Object());
    long most = Runtime.getRuntime().maxMemory() / 4;
    for (long done = 0; probe.get() != null && done < most; done += 64 << 10) sink = new byte[64 << 10];
    sink = null;
  }

  /** The resident size (VmRSS) in bytes, or -1. Allocates nothing once the file is open. */
  static long rss() {
    try {
      if (status == null) status = new RandomAccessFile("/proc/self/status", "r");
      status.seek(0);
      int n = 0;
      for (int r; n < BUFFER.length && (r = status.read(BUFFER, n, BUFFER.length - n)) > 0; ) n += r;
      for (int i = 0; i + VM_RSS.length < n; i++) {
        if (BUFFER[i] != 'V' || (i > 0 && BUFFER[i - 1] != '\n')) continue;
        boolean match = true;
        for (int k = 1; k < VM_RSS.length && match; k++) match = BUFFER[i + k] == VM_RSS[k];
        if (!match) continue;
        int j = i + VM_RSS.length;
        while (j < n && (BUFFER[j] == ' ' || BUFFER[j] == '\t')) j++;
        long kb = 0;
        int digits = 0;
        for (; j < n && BUFFER[j] >= '0' && BUFFER[j] <= '9'; j++, digits++) kb = kb * 10 + (BUFFER[j] - '0');
        return digits == 0 ? -1 : kb * 1024;
      }
    } catch (IOException | RuntimeException e) {
      // unreadable: no watching
    }
    return -1;
  }
}
