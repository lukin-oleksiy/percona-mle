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
 *
 * <p>The JDK's shared pools are the runtime's too: the virtual-thread scheduler's carriers and
 * the common pool's workers (parallel streams, CompletableFuture). A routine may use them for
 * parallel work; their threads stay idle for a while after it (30 s, 60 s), and the routine
 * cannot end them. Like the runtime-named ones, they are strangers while they run routine code.
 * A pool the routine makes is its own: it must close it.
 *
 * <p>A live virtual thread is always a stranger: neither the worker nor its language runtimes
 * start any, so it is a routine's (by its name alone it could pass for the runtime's, and before
 * it first runs its stack is empty).
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
  /** The threads alive when the worker started (before any routine): the JVM's own. */
  private final java.util.Set<Long> startup = new java.util.HashSet<>();

  ThreadWatch() {
    for (Thread t : allThreads()) startup.add(t.threadId());
  }

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
      if (t.isAlive() && (t.isVirtual() || !(system(t.getName()) || sharedPool(t)) || runsGuestCode(t)))
        return true;
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
    if (CONTAINER_ROOT != null) {
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

  /**
   * Whether t's stack has a frame of routine, library or DBA-jar code (JavaRoutines' loaders), or
   * no frame at all while it was started after the worker: a thread started but not yet run (a
   * busy machine) could run anything. (The JVM's own threads from the start, such as the Signal
   * Dispatcher, have no Java frames at all.)
   */
  private boolean runsGuestCode(Thread t) {
    if (t == Thread.currentThread()) return false;
    StackTraceElement[] stack = t.getStackTrace();
    if (stack.length == 0) return t.isAlive() && !startup.contains(t.threadId());
    for (StackTraceElement e : stack) {
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

  /** A worker of the common pool or a carrier of the virtual-thread scheduler (by its class). */
  private static boolean sharedPool(Thread t) {
    return t instanceof java.util.concurrent.ForkJoinWorkerThread w
        && (w.getPool() == java.util.concurrent.ForkJoinPool.commonPool()
            || t.getClass().getName().equals("jdk.internal.misc.CarrierThread"));
  }

  private static boolean system(String name) {
    for (String s : SYSTEM)
      if (name.startsWith(s)) return true;
    // A "pool-" thread may come from a Java routine: a stranger.
    return false;
  }
}
