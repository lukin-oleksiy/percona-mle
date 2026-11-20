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

import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.PrintStream;

/**
 * The worker's one way out when the host breaks the protocol (§1): a malformed or out-of-place
 * message ends the process at once, with a non-zero status. No guest code runs after it and none
 * sees it: nothing is thrown that guest code could catch.
 */
final class ProtocolFailure {
  private ProtocolFailure() {}

  /** The process's stderr itself: System.err is the routines' captured output. */
  private static final PrintStream LOG = new PrintStream(new FileOutputStream(FileDescriptor.err), true);

  /**
   * Ends the process. It does not return; the result type lets a caller write
   * {@code throw ProtocolFailure.exit(...)} where the compiler wants the method to end.
   */
  static Error exit(String why) {
    try {
      LOG.println("mle worker: protocol failure: " + why);
    } catch (Throwable ignored) {
      // the reason is for the log only
    }
    Runtime.getRuntime().halt(Wire.EXIT_PROTOCOL_FAILURE);
    throw new InternalError(why);
  }
}
