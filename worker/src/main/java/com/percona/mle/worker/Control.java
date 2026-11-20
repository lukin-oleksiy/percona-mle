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

/**
 * The control socket (fd 4, §6) and the worker's side of interruption (§11.1). The socket carries
 * INTERRUPT(call_seq) from the host; a datagram of another kind or length is ignored; its end of
 * file means the host is gone, and the worker ends.
 *
 * <p>Three variables under this object's lock, shared by the thread that serves requests and the
 * control thread: running (the call_seq of the interruptible top-level request being served),
 * last (the highest call_seq the worker began to serve) and pending (an INTERRUPT that came before
 * its request). An INTERRUPT is delivered to the running request only, once; one for a request not
 * begun yet waits for it; any other is ignored.
 *
 * <p>Delivering starts a thread that stops the request's guest code: Java code cooperatively
 * (Thread.interrupt; code that ignores it is ended by the host when its grace period is over),
 * JavaScript and Python with Context.interrupt. Each of its steps first checks, under the lock,
 * that the request still runs: once the request's status is fixed (end()), an interrupt that was
 * not acted on is cancelled, and none reaches a later request.
 */
final class Control implements Runnable {
  private final Thread main;
  private final Polyglot polyglot;
  private boolean guest;
  private Thread interrupter;
  private boolean isRunning;
  private long running;
  private boolean hasPending;
  private long pending;
  private long last;
  /** The running request's interrupt was delivered. */
  private boolean delivered;
  /**
   * The delivered interrupt was acted on: the runtime was told to stop, or the worker refused
   * work for it (SQL, a nested request), or guest code was told (mysql.sleep). The request then
   * ends INTERRUPTED; until then the interrupt can still be cancelled.
   */
  private boolean acted;

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

  private synchronized void interrupt(long seq) {
    if (isRunning && seq == running) {
      // The host may send several INTERRUPTs for one request: one delivery.
      if (!delivered) deliver();
    } else if (Long.compareUnsigned(seq, last) > 0) {
      hasPending = true;
      pending = seq;
    }
  }

  private void deliver() {
    delivered = true;
    stop();
  }

  /**
   * Java routines: Thread.interrupt. JS and Python routines: Context.interrupt, which stops guest
   * code at its next safepoint (done on another thread: it waits for the guest to stop). The
   * routine's language decides the first; interrupting a guest's thread as well delays the guest
   * interrupt (S-10), so the other comes only if the first did not stop the call: a Java routine
   * looping in JavaScript or Python (Mle.context()), a guest routine looping in Java (a DBA jar).
   */
  private void stop() {
    final long seq = running;
    final boolean guestFirst = guest;
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
  private synchronized boolean act(long seq) {
    if (!isRunning || running != seq) return false;
    acted = true;
    return true;
  }

  /**
   * Thread.interrupt of the main thread, only while request seq still runs: end() clears the
   * flag, so an interrupt after it would reach the next request.
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
    for (long left = ms; isRunning && running == seq && left > 0; left = until - System.currentTimeMillis()) {
      try {
        wait(left);
      } catch (InterruptedException e) {
        return false;
      }
    }
    return isRunning && running == seq;
  }

  /** The thread running a Context.interrupt for the ended request, if any (see Polyglot). */
  synchronized Thread takeInterrupter() {
    Thread t = interrupter;
    interrupter = null;
    return t;
  }

  /** Whether the running request's interrupt was delivered. */
  synchronized boolean delivered() {
    return delivered;
  }

  /**
   * Whether the running request's interrupt was delivered; if so, it counts as acted on: the
   * caller refuses work for it or tells guest code (mysql.sleep, the vector helper).
   */
  synchronized boolean refuse() {
    if (delivered) acted = true;
    return delivered;
  }

  /** Whether the running request's interrupt was delivered and acted on: its requests end INTERRUPTED. */
  synchronized boolean stopped() {
    return delivered && acted;
  }

  /**
   * The worker begins to serve top-level request seq (it has received it completely), on the main
   * thread. interruptible: PREPARE, CALL, PARSE_LIBRARY; guest: its code is JavaScript or Python.
   */
  synchronized void begin(long seq, boolean interruptible, boolean guest) {
    last = seq;
    boolean now = hasPending && pending == seq;
    hasPending = false;
    if (!interruptible) return;
    MemoryWatch.begin();
    isRunning = true;
    running = seq;
    this.guest = guest;
    delivered = false;
    acted = false;
    if (now) deliver();
  }

  /**
   * The interruptible top-level request ends: the worker has fixed the status of its RESULT.
   * True if its interrupt was delivered and acted on; one not acted on is cancelled here. Clears
   * the thread's interrupt flag.
   */
  synchronized boolean end() {
    if (isRunning) MemoryWatch.end();
    isRunning = false;
    notifyAll();  // an interrupter waiting to escalate
    boolean r = delivered && acted;
    delivered = false;
    acted = false;
    Thread.interrupted();
    return r;
  }
}
