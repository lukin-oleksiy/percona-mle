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
 * Captures System.out and System.err of routines; the output goes back to the host with each
 * RESULT, which keeps the console buffers (they survive a worker crash). Capped per message.
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

  private static final class Capture extends OutputStream {
    private final ByteArrayOutputStream buf = new ByteArrayOutputStream();
    private boolean truncated;

    @Override
    public synchronized void write(int b) {
      if (room(1)) buf.write(b);
    }

    @Override
    public synchronized void write(byte[] b, int off, int len) {
      int n = Math.min(len, LIMIT - buf.size());
      if (n > 0) buf.write(b, off, n);
      if (n < len) truncated = true;
    }

    private boolean room(int n) {
      if (buf.size() + n <= LIMIT) return true;
      truncated = true;
      return false;
    }

    synchronized byte[] take() {
      if (truncated) buf.writeBytes("\n[output truncated]\n".getBytes(StandardCharsets.UTF_8));
      byte[] r = buf.toByteArray();
      buf.reset();
      truncated = false;
      return r;
    }
  }
}
