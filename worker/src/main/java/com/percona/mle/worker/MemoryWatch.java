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
 * Native memory that only a Java GC gives back (exp note 30). GraalPy frees a native object (a
 * numpy array) when the Java object referring to it is collected; numpy allocates with malloc,
 * which does not make the JVM collect, so a loop of large temporaries grows until the worker's
 * cgroup limit kills it. GraalPy's own remedy (its "Python GC" task, above 4 GB) does not run in
 * the worker and would come too late.
 *
 * <p>While a top-level call runs, a thread reads the process's resident size every 10 ms and
 * forces a full GC when all of these hold:
 * <ul>
 *   <li>it is above half of the worker's memory limit (mle.memoryMax, from mle.memory_max);
 *   <li>it has grown by a tenth of the limit since the last GC (or since the lowest size seen);
 *   <li>at the rate it grew over the last ~50 ms, it would pass 85 % of the limit within 250 ms
 *       (a full GC takes ~150 ms), or it is above 70 % of the limit already.
 * </ul>
 * And above 85 % of the limit at most every 50 ms (or twice the last GC's time), whatever the
 * growth. Measured under load (16 busy threads on 16 CPUs): a forced GC took 250-800 ms, and
 * numpy's native code kept allocating meanwhile (a JVM pause doesn't stop native code); the frees
 * show only after the GC. The 70 % floor and the 85 % rule keep collecting there, and the
 * launcher sets the cgroup's memory.high to 90 % of memory.max, so that the kernel throttles the
 * allocating thread instead of killing the worker. numpy_memory.test failed 1 run in 3 under that
 * load before, none in 12 after (exp note 41).
 * The rate is what tells garbage from data (exp note 36): a numpy loop of large temporaries grows
 * at several GB/s and is collected as soon as it passes half the limit, as before; a call that
 * builds large live data (fetch_columns of 100k x VECTOR(768), 300 MB at ~1 GB/s) is left alone
 * until it nears the limit. Without the rate, that call paid one or two full GCs (~0.25 s).
 * Idle workers are left alone: RESET and the pool already trim them. The count goes to the host
 * with the worker's statistics (mle_worker_memory_gcs) and restarts at RESET. A check allocates
 * nothing: /proc/self/status is re-read through a file kept open, into a reused buffer.
 */
final class MemoryWatch implements Runnable {
  /** 10 ms: a numpy loop can allocate several GB/s (reading the size costs microseconds). */
  private static final long PERIOD_MS = Long.getLong("mle.memoryWatchMs", 10);
  private static final byte[] VM_RSS = "VmRSS:".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
  private static final AtomicLong GCS = new AtomicLong();
  private static volatile MemoryWatch instance;

  private final long limit;
  private final Thread thread;
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
  /** The resident size after the last GC, or the lowest seen since (bytes). */
  private long base = Long.MAX_VALUE;
  /** Recent samples (resident size, time) for the growth rate: a ring of WINDOW. */
  private static final int WINDOW = 6;
  private static final long HORIZON_NS = 250_000_000L;
  private final long[] sizes = new long[WINDOW];
  private final long[] times = new long[WINDOW];
  private int samples;
  /** When the last forced GC ended (System.nanoTime), and how long it took. */
  private long lastGc, lastGcNs;

  private MemoryWatch(long limit) {
    this.limit = limit;
    this.thread = new Thread(this, "mle-memory");
    thread.setDaemon(true);
  }

  /** Starts the watcher if the worker has a memory limit and can read its resident size. */
  static void start() {
    long limit = Long.getLong("mle.memoryMax", 0);
    if (limit <= 0 || rss() < 0) return;
    MemoryWatch w = new MemoryWatch(limit);
    w.thread.start();
    instance = w;
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
      if ((state & 1) != 0) check();
    }
  }

  private void check() {
    long rss = rss();
    if (rss < 0) return;
    long now = System.nanoTime();
    // A gap (the thread parked between calls): the old samples say nothing about this call.
    if (samples > 0 && now - times[(samples - 1) % WINDOW] > 5 * PERIOD_MS * 1_000_000L) samples = 0;
    sizes[samples % WINDOW] = rss;
    times[samples % WINDOW] = now;
    if (++samples == 2 * WINDOW) samples = WINDOW;  // the same ring positions, no overflow
    if (rss < base) base = rss;
    // Near the limit, collect again as soon as the last GC's frees could have shown, but spend at
    // most a third of the time in GC (a call that really holds that much data is slowed, not
    // stopped; the cgroup's memory.high throttles its allocations meanwhile).
    boolean near = rss > limit / 100 * 85 && now - lastGc > Math.max(50_000_000L, 2 * lastGcNs);
    if (near || (rss > limit / 2 && rss - base > limit / 10
        && (rss > limit / 100 * 70 || rss + growth(HORIZON_NS) > limit / 100 * 85))) {
      // Counted first: a GC can outlast the call (~150 ms, more on a busy host), and the call's
      // statistics (sent with its RESULT) must include it; RESET clears the count afterwards.
      GCS.incrementAndGet();
      long t0 = System.nanoTime();
      System.gc();
      long after = rss();
      base = after < 0 ? rss : after;
      lastGc = System.nanoTime();
      lastGcNs = lastGc - t0;
      samples = 0;
    }
  }

  /** How much the resident size grows in {@code ns} at its rate over the samples in the ring (0 if
   * it is not growing). */
  private long growth(long ns) {
    if (samples < 2) return 0;
    int newest = (samples - 1) % WINDOW;
    int oldest = samples <= WINDOW ? 0 : samples % WINDOW;
    long dt = times[newest] - times[oldest], ds = sizes[newest] - sizes[oldest];
    if (dt <= 0 || ds <= 0) return 0;
    return (long) ((double) ds / dt * ns);
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
