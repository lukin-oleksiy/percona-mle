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

import java.lang.management.ManagementFactory;
import java.lang.management.ThreadMXBean;

/**
 * Routines must not leave threads behind (15 §1, Q4): a thread that outlives its call keeps running
 * while the worker waits, is cached for the account's next session, or serves another session, and
 * KILL does not reach it. Python's and Java's thread APIs are refused, but Java routines and
 * reflection can still start threads; after each top-level request the worker looks for threads
 * that are not the runtime's own and, if one is alive, is retired (tainted) instead of kept.
 *
 * <p>The live-thread count is cheap; names are looked at only when it grew (the runtime starts
 * compiler and helper threads lazily).
 */
final class ThreadWatch {
  private static final ThreadMXBean MX = ManagementFactory.getThreadMXBean();
  /** The JVM's, Truffle's, GraalPy's and the worker's own threads. */
  private static final String[] SYSTEM = {"main", "mle-", "Reference Handler", "Finalizer",
      "Signal Dispatcher", "Common-Cleaner", "Notification Thread", "Attach Listener",
      "VirtualThread-unblocker", "TruffleCompilerThread", "Truffle", "python-actions-pool",
      "C API reference queue watcher", "process reaper", "JFR "};
  private int known = -1;

  /** True if a thread not belonging to the runtime is alive. */
  boolean strangers() {
    int n = MX.getThreadCount();
    if (n <= known) return false;
    for (Thread t : Thread.getAllStackTraces().keySet())
      if (t.isAlive() && !system(t.getName())) return true;
    known = n;  // only the runtime's threads: remember the count
    return false;
  }

  private static boolean system(String name) {
    for (String s : SYSTEM)
      if (name.startsWith(s)) return true;
    return false;
  }
}
