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
 * Entry of the MLE JVM worker, called by mle_worker (JNI) after the lockdown.
 *
 * <p>System properties: mle.class (worker class name for READY), mle.spin (spin iterations
 * before sleeping on the futex, default 2000), mle.prewarm (comma list of java, js, python,
 * numpy: loaded before READY), mle.maxDepth (nested calls, default 32), mle.python.venv.
 */
public final class Main {
  private Main() {}

  public static void run() {
    long t0 = System.nanoTime();
    Console console = new Console();
    try {
      Mailbox mb = Mailbox.open(Integer.getInteger("mle.spin", 2000));
      console.install();
      Options options = new Options();
      Polyglot polyglot = new Polyglot(console, options);
      Control control = new Control(Thread.currentThread(), polyglot);
      control.start();
      MemoryWatch.start();
      JavaRoutines java = new JavaRoutines();
      Dispatcher d = new Dispatcher(mb, java, polyglot, console, control, options,
          Integer.getInteger("mle.maxDepth", 32));
      SqlBridge.dispatcher = d;
      int status = Wire.OK;
      String info;
      try {
        String prewarm = System.getProperty("mle.prewarm", "");
        // Only what this build has (23): no Java routines or GraalJS in the native build.
        if (prewarm.contains("java") && !Polyglot.nativeImage()) java.prewarm();
        if (prewarm.contains("js") && Polyglot.JS) polyglot.context().eval("js", "1");
        // mle.pool_prewarm (15 §2): python, or numpy (Python and numpy).
        if (prewarm.contains("python") || prewarm.contains("numpy")) polyglot.prewarmPython(prewarm.contains("numpy"));
        // Starting leaves garbage and a grown heap; a worker often waits in the pool, so give
        // the memory back now (with the small-heap settings of the worker class).
        System.gc();
        info = "ready in " + (System.nanoTime() - t0) / 1_000_000 + " ms";
      } catch (Throwable t) {
        status = Wire.INTERNAL;
        // The causes too (an ExceptionInInitializerError says little by itself); the stack
        // goes to stderr (mysqld's error log with MLE_WORKER_STDERR=1).
        StringBuilder why = new StringBuilder("start failed: ").append(t);
        for (Throwable c = t.getCause(); c != null && c != c.getCause(); c = c.getCause())
          why.append("; caused by ").append(c);
        info = why.toString();
        t.printStackTrace();
      }
      d.ready(status, info);
      d.loop();
    } catch (Throwable t) {
      console.log().println("mle worker: fatal: " + t);
      t.printStackTrace(console.log());
      Runtime.getRuntime().halt(1);
    }
  }
}
