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

import java.nio.file.Files;
import java.nio.file.Path;
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
 * forces a full GC when it is above half of the worker's memory limit (mle.memoryMax, from
 * mle.memory_max) and has grown by a tenth of the limit since the last GC (or since the lowest
 * size seen). Idle workers are left alone: RESET and the pool already trim them. The count goes
 * to the host with the worker's statistics (mle_worker_memory_gcs) and restarts at RESET.
 */
final class MemoryWatch implements Runnable {
  /** 10 ms: a numpy loop can allocate several GB/s (reading the size costs microseconds). */
  private static final long PERIOD_MS = Long.getLong("mle.memoryWatchMs", 10);
  private static final Path STATUS = Path.of("/proc/self/status");
  private static final AtomicLong GCS = new AtomicLong();
  private static volatile MemoryWatch instance;

  private final long limit;
  private final Thread thread;
  private volatile boolean running;
  /** The resident size after the last GC, or the lowest seen since (bytes). */
  private long base = Long.MAX_VALUE;

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
    w.running = true;
    LockSupport.unpark(w.thread);
  }

  /** The top-level call ended. */
  static void end() {
    MemoryWatch w = instance;
    if (w != null) w.running = false;
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
    while (true) {
      if (!running) {
        LockSupport.park(this);
        continue;
      }
      try {
        Thread.sleep(PERIOD_MS);
      } catch (InterruptedException e) {
        Thread.interrupted();  // nothing interrupts this thread on purpose; go on
      }
      if (running) check();
    }
  }

  private void check() {
    long rss = rss();
    if (rss < 0) return;
    if (rss < base) base = rss;
    if (rss > limit / 2 && rss - base > limit / 10) {
      System.gc();
      GCS.incrementAndGet();
      long after = rss();
      base = after < 0 ? rss : after;
    }
  }

  /** The resident size (VmRSS) in bytes, or -1. */
  static long rss() {
    try {
      for (String line : Files.readAllLines(STATUS)) {
        if (!line.startsWith("VmRSS:")) continue;
        String kb = line.substring(6).trim();
        int space = kb.indexOf(' ');
        return Long.parseLong(space < 0 ? kb : kb.substring(0, space)) * 1024;
      }
    } catch (Exception e) {
      // unreadable: no watching
    }
    return -1;
  }
}
