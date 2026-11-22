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

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The control socket (fd 4, §6) and the worker's side of interruption (§11.1). The socket carries
 * INTERRUPT(call_seq) from the host; a datagram of another kind or length is ignored; its end of
 * file means the host is gone, and the worker ends.
 *
 * <p>The worker behaves as §11.1's table says of three variables under one lock: running (the
 * call_seq of the interruptible top-level request being served), last (the highest call_seq the
 * worker began to serve) and pending (an INTERRUPT that came before its request). An INTERRUPT is
 * delivered to the running request only, once; one for a request not begun yet waits for it; any
 * other is ignored. A request that is not interrupted takes no lock: the three are atomics.
 *
 * <ul>
 * <li>{@code state} is 0, or the running request's call_seq << 2 with the bits DELIVERED and
 * ACTED. The decision "this INTERRUPT is for the running request" is a compare-and-set of
 * DELIVERED on a state that names the request; the request's end is a get-and-set to 0. One of
 * the two comes first, so an interrupt never reaches a later request.
 * <li>The serving thread writes state, then last, then reads pending; the control thread reads
 * last and state, writes pending, then reads state again. All are volatile, so of a request that
 * begins while its INTERRUPT arrives at least one side sees the other, and the compare-and-set
 * lets one of them deliver.
 * </ul>
 *
 * <p>Delivering starts a thread that stops the request's guest code: Java code cooperatively
 * (Thread.interrupt; code that ignores it is ended by the host when its grace period is over),
 * JavaScript and Python with Context.interrupt. Each of its steps first sets ACTED by a
 * compare-and-set on a state that still names the request: once the request's status is fixed
 * (end()), an interrupt that was not acted on is cancelled. Only what follows a delivery uses this
 * object's lock: starting that thread, its Thread.interrupt of the serving thread, and the end of
 * a request whose interrupt was delivered, which waits there until neither is in progress.
 */
final class Control implements Runnable {
  private static final long DELIVERED = 1;
  /**
   * The delivered interrupt was acted on: the runtime was told to stop, or the worker refused
   * work for it (SQL, a nested request), or guest code was told (mysql.sleep). The request then
   * ends INTERRUPTED; until then the interrupt can still be cancelled.
   */
  private static final long ACTED = 2;

  private final Thread main;
  private final Polyglot polyglot;
  /** The serving thread's own: the running request's code is JavaScript or Python. */
  private boolean guest;
  /** The serving thread's own: an interruptible request is being served. */
  private boolean serving;
  /** Guarded by the lock: the thread that stops the request whose interrupt was delivered. */
  private Thread interrupter;
  private final AtomicLong state = new AtomicLong();
  private final AtomicLong pending = new AtomicLong();
  private volatile long last;

  Control(Thread main, Polyglot polyglot) {
    this.main = main;
    this.polyglot = polyglot;
  }

  void start() {
    Thread t = new Thread(this, "mle-control");
    t.setDaemon(true);
    t.start();
  }

  @Override
  public void run() {
    // Larger than an INTERRUPT, so that a datagram of another length is seen as one.
    MemorySegment buf = Arena.global().allocate(64);
    while (true) {
      long n = Native.read(Wire.CONTROL_FD, buf);
      if (n <= 0) Runtime.getRuntime().halt(0);  // the host closed its end or died
      if (n == Wire.CTL_INTERRUPT_SIZE && buf.get(ValueLayout.JAVA_BYTE, 0) == Wire.CTL_INTERRUPT)
        interrupt(buf.get(ValueLayout.JAVA_LONG_UNALIGNED, 1));
    }
  }

  private static boolean names(long s, long seq) {
    return s != 0 && s >>> 2 == seq;
  }

  /** INTERRUPT(seq) arrived (the control thread). */
  private void interrupt(long seq) {
    long was = last;
    if (deliver(seq)) return;
    if (Long.compareUnsigned(seq, was) <= 0) return;  // a request that is over, or none
    pending.set(seq);
    // The request may have begun meanwhile, before it could see pending.
    deliver(seq);
  }

  /**
   * Delivers the interrupt of request seq if that request is running and has none yet (the host
   * may send several INTERRUPTs for one request: one delivery). True if seq is the running request.
   */
  private boolean deliver(long seq) {
    long s = state.get();
    if (!names(s, seq)) return false;
    if ((s & DELIVERED) != 0) return true;
    synchronized (this) {
      // Under the lock, so that the request's end finds the interrupter it has to wait for.
      if (state.compareAndSet(s, s | DELIVERED)) stop(seq, guest);
      return true;
    }
  }

  /**
   * Java routines: Thread.interrupt. JS and Python routines: Context.interrupt, which stops guest
   * code at its next safepoint (done on another thread: it waits for the guest to stop). The
   * routine's language decides the first; interrupting a guest's thread as well delays the guest
   * interrupt (S-10), so the other comes only if the first did not stop the call: a Java routine
   * looping in JavaScript or Python (Mle.context()), a guest routine looping in Java (a DBA jar).
   */
  private void stop(long seq, boolean guestFirst) {
    org.graalvm.polyglot.Context c = polyglot.contextIfCreated();
    Thread t = new Thread(() -> {
      org.graalvm.polyglot.Context ctx = c;
      if (guestFirst) {
        // A first guest routine may still be building the context: wait for it while the call
        // runs (a repeated INTERRUPT is ignored, so this interrupter must not give up early).
        while (ctx == null && stillRunning(seq, 20)) ctx = polyglot.contextIfCreated();
        if (ctx == null || !act(seq) || interruptContext(ctx, 500)) return;
        if (interruptMain(seq)) interruptContext(ctx, 4500);
      } else {
        if (!interruptMain(seq)) return;
        if (stillRunning(seq, 500)) {
          if (ctx == null) ctx = polyglot.contextIfCreated();  // made by the Java code meanwhile
          if (ctx != null) interruptContext(ctx, 4500);
        }
      }
    }, "mle-interrupt");
    t.setDaemon(true);
    interrupter = t;
    t.start();
  }

  /** The interrupt of request seq is acted on now, if that request still runs. */
  private boolean act(long seq) {
    while (true) {
      long s = state.get();
      if (!names(s, seq) || (s & DELIVERED) == 0) return false;
      if ((s & ACTED) != 0 || state.compareAndSet(s, s | ACTED)) return true;
    }
  }

  /**
   * Thread.interrupt of the main thread, only while request seq still runs: end() clears the
   * flag, so an interrupt after it would reach the next request. Under the lock: a request that
   * ends with a delivered interrupt waits for it there.
   */
  private synchronized boolean interruptMain(long seq) {
    if (!act(seq)) return false;
    main.interrupt();
    return true;
  }

  /**
   * A Context.interrupt that starts after the request ended finds no thread in the context (the
   * main thread has left it: Polyglot.settleInterrupt) and returns at once.
   */
  private static boolean interruptContext(org.graalvm.polyglot.Context c, long ms) {
    try {
      c.interrupt(java.time.Duration.ofMillis(ms));
      return true;
    } catch (Exception e) {
      return false;  // timed out or the context is gone: escalate, or the host ends the worker
    }
  }

  /** Whether request seq is still running after up to ms (end() wakes the wait). */
  private synchronized boolean stillRunning(long seq, long ms) {
    long until = System.currentTimeMillis() + ms;
    for (long left = ms; names(state.get(), seq) && left > 0; left = until - System.currentTimeMillis()) {
      try {
        wait(left);
      } catch (InterruptedException e) {
        return false;
      }
    }
    return names(state.get(), seq);
  }

  /** The thread running a Context.interrupt for the ended request, if any (see Polyglot). */
  Thread takeInterrupter() {
    Thread t = ended;
    ended = null;
    return t;
  }

  /** The serving thread's own: the interrupter of the request that ended, until it is taken. */
  private Thread ended;

  /** Whether the running request's interrupt was delivered. */
  boolean delivered() {
    return (state.get() & DELIVERED) != 0;
  }

  /**
   * Whether the running request's interrupt was delivered; if so, it counts as acted on: the
   * caller refuses work for it or tells guest code (mysql.sleep, the vector helper).
   */
  boolean refuse() {
    while (true) {
      long s = state.get();
      if ((s & DELIVERED) == 0) return false;
      if ((s & ACTED) != 0 || state.compareAndSet(s, s | ACTED)) return true;
    }
  }

  /** Whether the running request's interrupt was delivered and acted on: its requests end INTERRUPTED. */
  boolean stopped() {
    return (state.get() & (DELIVERED | ACTED)) == (DELIVERED | ACTED);
  }

  /**
   * The worker begins to serve top-level request seq (it has received it completely), on the main
   * thread. interruptible: PREPARE, CALL, PARSE_LIBRARY; guest: its code is JavaScript or Python.
   */
  void begin(long seq, boolean interruptible, boolean guest) {
    if (interruptible) {
      MemoryWatch.begin();
      serving = true;
      this.guest = guest;
      state.set(seq << 2);
    }
    last = seq;
    long p = pending.get();
    if (p == 0) return;
    // An INTERRUPT that came before its request is delivered now; any other waited for nothing.
    pending.compareAndSet(p, 0);
    if (interruptible && p == seq) deliver(seq);
  }

  /**
   * The interruptible top-level request ends: the worker has fixed the status of its RESULT.
   * True if its interrupt was delivered and acted on; one not acted on is cancelled here. Clears
   * the thread's interrupt flag.
   */
  boolean end() {
    if (serving) MemoryWatch.end();
    serving = false;
    long s = state.getAndSet(0);
    if ((s & DELIVERED) != 0) {
      // Nothing of the interrupt is in progress once the lock is held: its interrupter is
      // started, and a Thread.interrupt of this thread, if one was decided, is done.
      synchronized (this) {
        ended = interrupter;
        interrupter = null;
        notifyAll();  // an interrupter waiting to escalate
      }
    }
    Thread.interrupted();
    return (s & (DELIVERED | ACTED)) == (DELIVERED | ACTED);
  }
}
