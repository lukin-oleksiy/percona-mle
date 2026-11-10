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

import org.graalvm.polyglot.Context;

/**
 * Build/installation step: creates the polyglot Context once outside the lockdown, so that the
 * languages' resources (e.g. the Python standard library) are extracted into the resource cache
 * given by -Dpolyglot.engine.userResourceCache. Workers can only read it.
 */
public final class Setup {
  private Setup() {}

  public static void main(String[] args) {
    int status = 0;
    try (Context c = Polyglot.builder(null).build()) {
      c.eval("js", "1");
      c.eval("python", "import json, decimal, datetime, array");
    } catch (Throwable t) {
      t.printStackTrace();
      status = 1;
    }
    // GraalPy leaves non-daemon threads behind.
    Runtime.getRuntime().halt(status);
  }
}
