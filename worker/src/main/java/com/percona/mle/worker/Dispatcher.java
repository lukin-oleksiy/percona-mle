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

import com.percona.mle.api.MleSqlException;
import com.percona.mle.api.SqlResult;
import java.lang.foreign.Arena;
import java.util.ArrayList;
import java.util.List;

/**
 * Handles the host's requests (PREPARE, CALL, DROP, RESET) on the worker's main thread and posts
 * their RESULTs; runs Mle.sql round trips, serving nested requests from the host meanwhile.
 *
 * <p>Nesting: a routine's SQL may call another routine of this worker. The host then sends CALL
 * (or PREPARE) with depth + 1 while the worker waits for SQL_RESULT; it is handled recursively.
 */
final class Dispatcher {
  private final Mailbox mb;
  private final MessageWriter w;
  private final JavaRoutines java;
  private final Console console;
  private final Control control;
  private final Thread thread;
  private final int maxDepth;
  private int depth;
  private long seq;
  private boolean tainted;

  record ErrorInfo(String language, String message, StackTraceElement[] frames, int sqlErrno, String sqlState) {}

  Dispatcher(Mailbox mb, JavaRoutines java, Console console, Control control, int maxDepth) {
    this.mb = mb;
    this.w = new MessageWriter(mb.arena);
    this.java = java;
    this.console = console;
    this.control = control;
    this.thread = Thread.currentThread();
    this.maxDepth = maxDepth;
  }

  void ready(int status, String info) {
    w.reset();
    w.u32(Wire.VERSION);
    w.str(System.getProperty("mle.class", "jvm"));
    w.u32(1);
    w.str("java");
    w.str("Java " + Runtime.version() + " (" + System.getProperty("java.vm.name") + ")");
    w.str(info);
    mb.stats(Wire.STATE_IDLE, false);
    mb.send(Wire.READY, status, 0, 0, w);
  }

  void loop() {
    while (true) {
      Mailbox.Message m = mb.receive();
      if (m.depth() != 0) throw new IllegalStateException("top-level message at depth " + m.depth());
      handle(m);
    }
  }

  /** Handles one request and posts its RESULT. */
  private void handle(Mailbox.Message m) {
    int savedDepth = depth;
    long savedSeq = seq;
    depth = m.depth();
    if (depth == 0) seq = m.callSeq();
    try {
      switch (m.op()) {
        case Wire.PREPARE -> prepare(m);
        case Wire.CALL -> call(m);
        case Wire.DROP -> {
          MessageReader r = new MessageReader(m.body());
          java.drop(r.u64());
          r.finish();
          result(Wire.OK, null, null, null);
        }
        case Wire.RESET -> {
          java.reset();
          result(Wire.OK, null, null, null);
        }
        default -> result(Wire.INTERNAL, null, null, internal("unexpected message " + m.op()));
      }
    } finally {
      depth = savedDepth;
      seq = savedSeq;
    }
  }

  private void prepare(Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    long id = r.u64();
    String language = r.str();
    String entry = r.str();
    String source = r.str();
    int n = r.count();
    List<JavaRoutines.Param> params = new ArrayList<>(n);
    for (int i = 0; i < n; i++) params.add(new JavaRoutines.Param(r.str(), r.u8(), Values.readParamType(r)));
    Values.ParamType ret = r.flag() ? Values.readParamType(r) : null;
    r.finish();

    if (!language.equals("java")) {
      result(Wire.GUEST_ERROR, null, null, guest(language, "the language " + language + " is not available in this worker"));
      return;
    }
    for (JavaRoutines.Param p : params)
      if (p.mode() != Wire.MODE_IN) {
        result(Wire.GUEST_ERROR, null, null, guest("java", "OUT and INOUT parameters are not supported yet"));
        return;
      }
    mb.stats(Wire.STATE_COMPILING, tainted);
    try {
      java.prepare(id, entry, source, params, ret);
      result(Wire.OK, null, null, null);
    } catch (JavaRoutines.CompileException e) {
      result(Wire.GUEST_ERROR, null, null, guest("java", e.getMessage()));
    }
  }

  private void call(Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    long id = r.u64();
    r.u32();  // events: no statement-scoped state yet
    int n = r.count();
    Object[] args = new Object[n];
    for (int i = 0; i < n; i++) args[i] = Values.read(r);
    r.finish();
    JavaRoutines.Routine routine = java.get(id);
    if (routine == null || routine.converters().length != n) {
      result(Wire.INTERNAL, null, null, internal("routine " + id + " is not prepared with " + n + " parameters"));
      return;
    }

    boolean top = depth == 0;
    if (top) control.begin(seq);
    mb.stats(Wire.STATE_RUNNING, tainted);
    // The call's arena owns MemorySegment arguments; closing it makes them unusable afterwards.
    try (Arena callArena = Arena.ofConfined()) {
      Object ret = null;
      Throwable failure = null;
      try {
        ret = java.invoke(routine, args, callArena);
      } catch (Throwable t) {
        failure = t;
      }
      boolean interrupted = top && control.end();
      if (failure == null) result(Wire.OK, ret, routine.ret(), null);
      else failed(failure, interrupted);
    }
  }

  private void failed(Throwable t, boolean interrupted) {
    if (t instanceof OutOfMemoryError || t instanceof StackOverflowError) {
      tainted = true;  // the host retires this worker after the call
      result(Wire.EXHAUSTED, null, null, new ErrorInfo("java", t.toString(), userFrames(t), 0, ""));
    } else if (interrupted) {
      result(Wire.INTERRUPTED, null, null, guest("java", "interrupted"));
    } else if (t instanceof MleSqlException e) {
      result(Wire.GUEST_ERROR, null, null,
          new ErrorInfo("java", e.getMessage(), userFrames(t), e.getErrorCode(), e.getSqlState()));
    } else {
      String message = t instanceof Values.ConversionException ? t.getMessage() : describe(t);
      result(Wire.GUEST_ERROR, null, null, new ErrorInfo("java", message, userFrames(t), 0, ""));
    }
  }

  private static String describe(Throwable t) {
    StringBuilder s = new StringBuilder(t.toString());
    for (Throwable c = t.getCause(); c != null && c != t; c = c.getCause()) s.append("; caused by ").append(c);
    return s.toString();
  }

  /** The routine's own frames (classes from routine loaders). */
  private static StackTraceElement[] userFrames(Throwable t) {
    List<StackTraceElement> frames = new ArrayList<>();
    for (StackTraceElement e : t.getStackTrace()) {
      String loader = e.getClassLoaderName();
      if (loader != null && loader.startsWith(JavaRoutines.LOADER_PREFIX)) frames.add(e);
      if (frames.size() == Wire.MAX_STACK_FRAMES) break;
    }
    return frames.toArray(new StackTraceElement[0]);
  }

  private static ErrorInfo guest(String language, String message) {
    return new ErrorInfo(language, message, new StackTraceElement[0], 0, "");
  }

  private static ErrorInfo internal(String message) {
    return new ErrorInfo("", message, new StackTraceElement[0], 0, "");
  }

  /** Posts a RESULT. A return value that cannot be converted turns into a guest error. */
  private void result(int status, Object ret, Values.ParamType retType, ErrorInfo error) {
    try {
      writeResult(ret, retType, error);
    } catch (Values.ConversionException e) {
      status = Wire.GUEST_ERROR;
      writeResult(null, null, guest("java", "return value: " + e.getMessage()));
    }
    mb.stats(depth == 0 ? Wire.STATE_IDLE : Wire.STATE_RUNNING, tainted);
    mb.send(Wire.RESULT, status, depth, seq, w);
  }

  private void writeResult(Object ret, Values.ParamType retType, ErrorInfo error) {
    w.reset();
    if (retType != null && error == null) {
      w.u8(1);
      Values.writeAs(w, ret, retType);
    } else {
      w.u8(0);
    }
    w.u32(0);  // OUT parameters: Phase 8
    w.bytes(console.takeOut());
    w.bytes(console.takeErr());
    if (error == null) {
      w.u8(0);
      return;
    }
    w.u8(1);
    w.str(error.language());
    w.str(error.message());
    w.u32(error.frames().length);
    for (StackTraceElement f : error.frames()) {
      w.str(f.getClassName() + "." + f.getMethodName());
      w.str(f.getFileName() == null ? "" : f.getFileName());
      w.u32(Math.max(0, f.getLineNumber()));
      w.u32(0);
    }
    w.u32(error.sqlErrno());
    w.str(error.sqlState() == null ? "" : error.sqlState());
  }

  // ------------------------------------------------------------ SQL

  SqlResult sql(String statement, Object[] params) {
    if (Thread.currentThread() != thread)
      throw new IllegalStateException("Mle.sql must be called on the routine's thread");
    w.reset();
    w.str(statement);
    w.u32(params.length);
    for (Object p : params) Values.writeParam(w, p);
    w.u32(0);
    mb.send(Wire.SQL, Wire.OK, depth, seq, w);
    while (true) {
      Mailbox.Message m = mb.receive();
      if (m.op() == Wire.SQL_RESULT && m.depth() == depth) return sqlResult(new MessageReader(m.body()));
      boolean nested = m.op() == Wire.CALL || m.op() == Wire.PREPARE || m.op() == Wire.DROP;
      if (nested && m.depth() == depth + 1 && m.depth() <= maxDepth) {
        handle(m);
        continue;
      }
      throw new IllegalStateException("unexpected message " + m.op() + " at depth " + m.depth() + " during SQL");
    }
  }

  private static SqlResult sqlResult(MessageReader r) {
    int kind = r.u8();
    switch (kind) {
      case Wire.SQL_ERROR -> {
        int errno = (int) r.u32();
        String sqlState = r.str();
        String message = r.str();
        throw new MleSqlException(errno, sqlState, message);
      }
      case Wire.SQL_OK -> {
        long affected = r.u64();
        long insertId = r.u64();
        List<String> warnings = warnings(r);
        r.finish();
        return new SqlResult(List.of(), List.of(), affected, insertId, warnings);
      }
      case Wire.SQL_ROWS -> {
        int ncols = r.count();
        List<String> columns = new ArrayList<>(ncols);
        for (int i = 0; i < ncols; i++) {
          columns.add(r.str());
          r.str();  // table
          Values.readParamType(r);
        }
        long nrows = r.u32();
        List<Object[]> rows = new ArrayList<>();
        for (long i = 0; i < nrows; i++) {
          Object[] row = new Object[ncols];
          for (int c = 0; c < ncols; c++) row[c] = Values.toJava(Values.read(r));
          rows.add(row);
        }
        r.flag();  // more rows: fetching comes with the Phase 5 SQL API
        List<String> warnings = warnings(r);
        r.finish();
        return new SqlResult(List.copyOf(columns), rows, 0, 0, warnings);
      }
      default -> throw new IllegalStateException("bad SQL result kind " + kind);
    }
  }

  private static List<String> warnings(MessageReader r) {
    int n = r.count();
    List<String> list = new ArrayList<>(n);
    for (int i = 0; i < n; i++) list.add(r.u32() + " " + r.u32() + " " + r.str());
    return list;
  }
}
