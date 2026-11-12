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
import java.util.List;

/**
 * Build step: the training run for the JDK AOT cache (mle.aot). Runs what a worker runs — the
 * codec, javac and Java routines, the polyglot Context with JS, Python and numpy routines — so
 * that their classes are loaded and linked ahead of time and their methods profiled. The JVM must
 * end normally (System.exit) for the AOT configuration to be written.
 */
public final class Train {
  private Train() {}

  public static void main(String[] args) throws Throwable {
    int rounds = Integer.getInteger("mle.train.rounds", 20000);
    Values.ParamType i64 = new Values.ParamType(Wire.T_I64, false, 0, 0, 0);
    Values.ParamType str = new Values.ParamType(Wire.T_STRING, false, 0, 0, 0);
    Values.ParamType vec = new Values.ParamType(Wire.T_VECTOR, false, 0, 0, 0);
    Values.ParamType f64 = new Values.ParamType(Wire.T_F64, false, 0, 0, 0);
    List<JavaRoutines.Param> threeArgs = List.of(new JavaRoutines.Param("a", 0, i64),
        new JavaRoutines.Param("b", 0, str), new JavaRoutines.Param("c", 0, f64));

    // Codec: encode and decode the values of a typical call.
    try (Arena arena = Arena.ofConfined()) {
      MemorySegment buf = arena.allocate(1 << 16, 16);
      MessageWriter w = new MessageWriter(buf);
      for (int i = 0; i < rounds; i++) {
        w.reset();
        Values.writeParam(w, (long) i);
        Values.writeParam(w, "hello");
        Values.writeParam(w, 2.5);
        Values.writeParam(w, new float[] {1, 2, 3});
        MessageReader r = new MessageReader(w.body());
        for (int k = 0; k < 4; k++) Values.toJava(Values.read(r));
      }
    }

    // Java routines.
    JavaRoutines java = new JavaRoutines();
    java.prepare(1, "", "T.f", "public class T { public static long f(long a, String b, double c) { "
        + "return a + b.length() + (long) c; } }", threeArgs, i64);
    JavaRoutines.Routine jr = java.get(1);
    try (Arena arena = Arena.ofConfined()) {
      for (int i = 0; i < rounds; i++) java.invoke(jr, new Object[] {1L, "hello", 2.0}, arena, new Object[3]);
    }
    java.prepare(5, "jf", "", "return a + b.length() + c.longValue();", threeArgs, i64);
    try (Arena arena = Arena.ofConfined()) {
      for (int i = 0; i < rounds; i++) java.invoke(java.get(5), new Object[] {1L, "hello", 2.0}, arena, new Object[3]);
    }

    // JS and Python routines, numpy when the venv is there.
    Polyglot polyglot = new Polyglot(new Console(), new Options());
    polyglot.prepare(2, "js", "", "f", "function f(a, b, c) { return a + b.length + c; }", threeArgs, i64, List.of());
    // The body forms (CREATE ... AS $$ body $$).
    polyglot.prepare(6, "js", "jf", "", "return a + b.length + c;", threeArgs, i64, List.of());
    // Python only where it is installed (the AOT cache is made for the installed languages).
    if (Polyglot.PYTHON) {
      polyglot.prepare(3, "python", "", "f", "def f(a, b, c):\n    return a + len(b) + int(c)\n", threeArgs, i64, List.of());
      polyglot.prepare(7, "python", "pf", "", "return a + len(b) + int(c)", threeArgs, i64, List.of());
    }
    Object[] outs = new Object[3];
    for (int i = 0; i < rounds; i++) {
      for (int id : Polyglot.PYTHON ? new int[] {2, 3, 6, 7} : new int[] {2, 6})
        polyglot.invoke(polyglot.get(id), new Object[] {1L, "hello", 2.0}, outs);
    }
    if (Polyglot.PYTHON && !System.getProperty("mle.python.venv", "").isEmpty()) {
      polyglot.prepare(4, "python", "", "f", "import numpy as np\ndef f(v):\n    return float(np.dot(v, v))\n",
          List.of(new JavaRoutines.Param("v", 0, vec)), f64, List.of());
      polyglot.context().eval("python", "import numpy; numpy.ones(768) @ numpy.ones(768)");
    }
    System.out.println("mle training done");
    System.exit(0);  // GraalPy leaves non-daemon threads; exit writes the AOT configuration
  }
}
