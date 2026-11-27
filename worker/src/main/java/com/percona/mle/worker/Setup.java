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

import java.nio.file.Path;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.Value;

/**
 * Build/installation step: creates the polyglot Context once outside the lockdown, so that the
 * languages' resources (e.g. the Python standard library) are extracted into the resource cache
 * given by -Dpolyglot.engine.userResourceCache. Workers can only read it.
 *
 * <p>{@code --compile-python <venv>}: GraalPy's bytecode for the venv's packages (numpy). The
 * venv's installer writes none, and workers cannot write it (python.DontWriteBytecodeFlag, a
 * read-only file system), so without it every worker compiled numpy's sources on import: about
 * 0.7 s of a new connection's first {@code import numpy} (exp note 69). Unchecked-hash bytecode:
 * valid whatever the installed files' times.
 */
public final class Setup {
  private Setup() {}

  public static void main(String[] args) {
    int status = 0;
    if (args.length == 2 && args[0].equals("--compile-python")) {
      try {
        status = compilePython(Path.of(args[1])) ? 0 : 1;
      } catch (Throwable t) {
        t.printStackTrace();
        status = 1;
      }
      Runtime.getRuntime().halt(status);
    }
    try (Context c = Polyglot.builder(null).build()) {
      c.eval("js", "1");
      if (Polyglot.PYTHON) c.eval("python", "import json, decimal, datetime, array");
    } catch (Throwable t) {
      t.printStackTrace();
      status = 1;
    }
    // GraalPy leaves non-daemon threads behind.
    Runtime.getRuntime().halt(status);
  }

  /** Compiles the site-packages of the venv (its bin/python names it, as for workers). */
  private static boolean compilePython(Path venv) {
    try (Context c = Context.newBuilder("python").allowAllAccess(true)
        .option("python.Executable", venv.resolve("bin/python").toString())
        .option("python.ForceImportSite", "true")
        .option("python.PosixModuleBackend", "java")
        .build()) {
      Value ok = c.eval("python", """
          import compileall, py_compile, site
          all([compileall.compile_dir(d, quiet=1,
                   invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
               for d in site.getsitepackages()])
          """);
      return ok.asBoolean();
    }
  }
}
