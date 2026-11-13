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

/**
 * Routines must not leave threads behind (15 §1, Q4): a thread that outlives its call keeps running
 * while the worker waits, is cached for the account's next session, or serves another session, and
 * KILL does not reach it. Python's thread API is refused and JavaScript/Python cannot reach Java's
 * thread classes; Java code (Java routines, DBA jars) still can. The worker looks for threads that
 * are not the runtime's own at RESET (which decides whether it is kept for reuse) and, during a
 * session, after a top-level request at most every few seconds; with one, it is retired (tainted).
 *
 * <p>Plain thread enumeration, no java.lang.management (JMX is slow to start in every worker and
 * not in the AOT cache).
 */
final class ThreadWatch {
  /** The JVM's, Truffle's, GraalPy's and the worker's own threads. */
  private static final String[] SYSTEM = {"main", "mle-", "Reference Handler", "Finalizer",
      "Signal Dispatcher", "Common-Cleaner", "Notification Thread", "Attach Listener",
      "VirtualThread-unblocker", "TruffleCompilerThread", "Truffle", "python-actions-pool",
      "C API reference queue watcher", "process reaper", "JFR "};
  private static final long PERIOD_NS = 5_000_000_000L;
  private static final ThreadGroup ROOT = root();
  private long next;

  /** After a top-level request: strangers(), at most once per period. */
  boolean strangersNow() {
    long now = System.nanoTime();
    if (now - next < 0) return false;
    next = now + PERIOD_NS;
    return strangers();
  }

  /** True if a thread not belonging to the runtime is alive. */
  boolean strangers() {
    Thread[] all = new Thread[ROOT.activeCount() + 16];
    int n = ROOT.enumerate(all, true);
    for (int i = 0; i < n; i++)
      if (all[i].isAlive() && !system(all[i].getName())) return true;
    return false;
  }

  private static ThreadGroup root() {
    ThreadGroup g = Thread.currentThread().getThreadGroup();
    while (g.getParent() != null) g = g.getParent();
    return g;
  }

  private static boolean system(String name) {
    for (String s : SYSTEM)
      if (name.startsWith(s)) return true;
    // The Python-only native build (23) runs executor threads of its own ("pool-N-thread-M")
    // and GraalPy's native-memory "Python GC". No routine can start a thread there (Python's
    // thread API is refused; no Java routines, no JavaScript), so they are the runtime's.
    // On the JVM a "pool-" thread may come from a Java routine: still a stranger there.
    return Polyglot.nativeImage() && (name.startsWith("pool-") || name.startsWith("Python GC"));
  }
}
