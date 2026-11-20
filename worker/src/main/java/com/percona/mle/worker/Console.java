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

import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;

/**
 * Captures System.out and System.err of routines. The RESULT of a PREPARE or CALL carries
 * everything printed since the last one that did, at whatever depth (§9.4); output printed at
 * another time waits for the next, and RESET discards what waits. Each stream keeps at most
 * LIMIT bytes for a RESULT: the end of what was printed.
 */
final class Console {
  static final int LIMIT = 1 << 20;

  private final Capture out = new Capture();
  private final Capture err = new Capture();
  private final PrintStream originalErr = System.err;

  void install() {
    System.setOut(new PrintStream(out, true, StandardCharsets.UTF_8));
    System.setErr(new PrintStream(err, true, StandardCharsets.UTF_8));
  }

  /** The capture streams, for the polyglot Context's out and err. */
  OutputStream outStream() {
    return out;
  }

  OutputStream errStream() {
    return err;
  }

  /** The worker's own diagnostics (stderr of the process: /dev/null unless inherited). */
  PrintStream log() {
    return originalErr;
  }

  private boolean cleared;

  /** console.clear(): drops the output so far, and the host's (RESULT flag). */
  synchronized void clear() {
    System.out.flush();
    System.err.flush();
    out.take();
    err.take();
    cleared = true;
  }

  /** RESET: what waits for a RESULT is dropped, a console.clear() too. */
  synchronized void discard() {
    System.out.flush();
    System.err.flush();
    out.take();
    err.take();
    cleared = false;
  }

  synchronized boolean takeCleared() {
    boolean c = cleared;
    cleared = false;
    return c;
  }

  byte[] takeOut() {
    System.out.flush();
    return out.take();
  }

  byte[] takeErr() {
    System.err.flush();
    return err.take();
  }

  private static final byte[] EMPTY = new byte[0];

  /** Keeps the last LIMIT bytes written: up to twice as many are held before the older are dropped. */
  private static final class Capture extends OutputStream {
    private final ByteArrayOutputStream buf = new ByteArrayOutputStream();

    @Override
    public synchronized void write(int b) {
      buf.write(b);
      trim();
    }

    @Override
    public synchronized void write(byte[] b, int off, int len) {
      if (len > LIMIT) {  // only its end can stay
        off += len - LIMIT;
        len = LIMIT;
        buf.reset();
      }
      buf.write(b, off, len);
      trim();
    }

    private void trim() {
      if (buf.size() <= 2 * LIMIT) return;
      byte[] all = buf.toByteArray();
      buf.reset();
      buf.write(all, all.length - LIMIT, LIMIT);
    }

    synchronized byte[] take() {
      int n = buf.size();
      if (n == 0) return EMPTY;  // the usual case: no garbage per call
      byte[] all = buf.toByteArray();
      buf.reset();
      return n <= LIMIT ? all : java.util.Arrays.copyOfRange(all, n - LIMIT, n);
    }
  }
}
