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
 * The control socket (fd 4): INTERRUPT(call_seq) and SHUTDOWN from the host; EOF means the host
 * is gone, and the worker ends.
 *
 * <p>An INTERRUPT applies only to the top-level call with that sequence number. One that arrives
 * before its call has started (e.g. while the worker is still starting) is remembered and applied
 * when the call begins. Java code is interrupted cooperatively (Thread.interrupt); code that
 * ignores it is ended by the host with SIGKILL after the grace period.
 */
final class Control implements Runnable {
  private final Thread main;
  private final Polyglot polyglot;
  private boolean guest;
  private Thread interrupter;
  private long running = -1;
  private long pending = -1;
  private long last;
  private boolean interrupted;

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
    MemorySegment buf = Arena.global().allocate(64);
    while (true) {
      long n = Native.read(Wire.CONTROL_FD, buf);
      if (n <= 0) Runtime.getRuntime().halt(0);  // the host closed the session or died
      int kind = buf.get(ValueLayout.JAVA_BYTE, 0);
      if (kind == Wire.CTL_SHUTDOWN) Runtime.getRuntime().halt(0);
      if (kind == Wire.CTL_INTERRUPT && n >= 9) interrupt(buf.get(ValueLayout.JAVA_LONG_UNALIGNED, 1));
    }
  }

  private synchronized void interrupt(long seq) {
    if (seq == running) {
      interrupted = true;
      stop();
    } else if (seq > last) {
      pending = seq;
    }
  }

  /**
   * Java routines: Thread.interrupt. JS and Python routines: Context.interrupt, which stops guest
   * code at its next safepoint (done on another thread: it waits for the guest to stop). Only one of
   * the two: interrupting a guest's thread as well delays the guest interrupt (S-10).
   */
  private void stop() {
    if (!guest) {
      main.interrupt();
      return;
    }
    org.graalvm.polyglot.Context c = polyglot.contextIfCreated();
    if (c == null) return;
    Thread t = new Thread(() -> {
      try {
        c.interrupt(java.time.Duration.ofSeconds(5));
      } catch (Exception e) {
        // timed out or the context is gone: the host's SIGKILL covers it
      }
    }, "mle-interrupt");
    t.setDaemon(true);
    interrupter = t;
    t.start();
  }

  /** The thread running a Context.interrupt for the ended call, if any (see Polyglot). */
  synchronized Thread takeInterrupter() {
    Thread t = interrupter;
    interrupter = null;
    return t;
  }

  /** Whether the running call was interrupted (polled by long host waits, e.g. mysql.sleep). */
  synchronized boolean isInterrupted() {
    return interrupted;
  }

  /** A top-level call starts (on the main thread); guest: a JS or Python routine. */
  synchronized void begin(long seq, boolean guest) {
    running = seq;
    this.guest = guest;
    interrupted = false;
    if (pending == seq) {
      pending = -1;
      interrupted = true;
      stop();
    }
  }

  /** The top-level call ended; true if it was interrupted. Clears the thread's interrupt flag. */
  synchronized boolean end() {
    last = Math.max(last, running);
    running = -1;
    boolean r = interrupted;
    interrupted = false;
    Thread.interrupted();
    return r;
  }
}
