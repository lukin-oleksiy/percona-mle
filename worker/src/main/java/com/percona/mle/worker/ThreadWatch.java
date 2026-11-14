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
 * <p>No java.lang.management (JMX is slow to start in every worker and not in the AOT cache). The
 * JDK's thread containers (jdk.internal.vm, exported to the worker by its JVM options) also list
 * virtual threads, which thread groups do not; without them, platform threads only.
 *
 * <p>A thread named like the runtime's is still a stranger when its stack holds code of a routine,
 * a library or a DBA jar (their class loaders' names): a routine cannot pass its thread off as
 * the runtime's by its name alone. Not caught: such a thread idle in JDK code only (e.g. a pool
 * the routine made, with runtime-like names), until it runs routine code again.
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
    for (Thread t : allThreads())
      if (t.isAlive() && (!system(t.getName()) || runsGuestCode(t))) return true;
    return false;
  }

  private static final java.lang.reflect.Method CONTAINER_ROOT, CONTAINER_THREADS, CONTAINER_CHILDREN;

  static {
    java.lang.reflect.Method root = null, threads = null, children = null;
    try {
      Class<?> containers = Class.forName("jdk.internal.vm.ThreadContainers");
      Class<?> container = Class.forName("jdk.internal.vm.ThreadContainer");
      root = containers.getMethod("root");
      threads = container.getMethod("threads");
      children = container.getMethod("children");
      root.invoke(null);  // fails here if the package is not exported to us
    } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
      root = null;
    }
    CONTAINER_ROOT = root;
    CONTAINER_THREADS = threads;
    CONTAINER_CHILDREN = children;
  }

  /** Live threads, virtual ones included when the thread containers can be read. */
  private static java.util.List<Thread> allThreads() {
    if (CONTAINER_ROOT != null && !Polyglot.nativeImage()) {
      try {
        java.util.List<Thread> out = new java.util.ArrayList<>();
        collect(CONTAINER_ROOT.invoke(null), out, 0);
        return out;
      } catch (ReflectiveOperationException | RuntimeException e) {
        // fall back to the thread groups
      }
    }
    Thread[] all = new Thread[ROOT.activeCount() + 16];
    int n = ROOT.enumerate(all, true);
    return java.util.Arrays.asList(all).subList(0, n);
  }

  /** The container's threads and its child containers' (executors, structured scopes). */
  private static void collect(Object container, java.util.List<Thread> out, int depth)
      throws ReflectiveOperationException {
    ((java.util.stream.Stream<?>) CONTAINER_THREADS.invoke(container)).forEach(t -> out.add((Thread) t));
    if (depth >= 32) return;
    for (Object child : ((java.util.stream.Stream<?>) CONTAINER_CHILDREN.invoke(container)).toList())
      collect(child, out, depth + 1);
  }

  /** Whether t's stack has a frame of routine, library or DBA-jar code (JavaRoutines' loaders). */
  private static boolean runsGuestCode(Thread t) {
    if (Polyglot.nativeImage() || t == Thread.currentThread()) return false;  // no Java code there
    for (StackTraceElement e : t.getStackTrace()) {
      String loader = e.getClassLoaderName();
      if (loader != null && (loader.startsWith(JavaRoutines.LOADER_PREFIX)
          || loader.startsWith(JavaRoutines.LIBRARY_LOADER_PREFIX) || loader.equals(JavaRoutines.PACKAGE_LOADER)))
        return true;
    }
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
