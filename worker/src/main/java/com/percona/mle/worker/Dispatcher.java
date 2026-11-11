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

import com.percona.mle.api.MleSql;
import com.percona.mle.api.MleSqlException;
import java.lang.foreign.Arena;
import java.util.ArrayList;
import java.util.List;
import org.graalvm.polyglot.PolyglotException;

/**
 * Handles the host's requests (PREPARE, CALL, DROP, RESET) on the worker's main thread and posts
 * their RESULTs; runs SQL round trips, serving nested requests from the host meanwhile.
 *
 * <p>Nesting: a routine's SQL may call another routine of this worker. The host then sends CALL
 * (or PREPARE) with depth + 1 while the worker waits for SQL_RESULT; it is handled recursively.
 */
final class Dispatcher {
  private final Mailbox mb;
  private final MessageWriter w;
  private final JavaRoutines java;
  private final Polyglot polyglot;
  private final Console console;
  private final Control control;
  private final Options options;
  private final Thread thread;
  private final int maxDepth;
  private int depth;
  private long seq;
  private boolean tainted;

  record Frame(String function, String file, int line, int column, int endColumn) {}

  record ErrorInfo(String language, String message, List<Frame> frames, int sqlErrno, String sqlState) {}

  Dispatcher(Mailbox mb, JavaRoutines java, Polyglot polyglot, Console console, Control control, Options options,
      int maxDepth) {
    this.mb = mb;
    this.w = new MessageWriter(mb.arena);
    this.java = java;
    this.polyglot = polyglot;
    this.console = console;
    this.control = control;
    this.options = options;
    this.thread = Thread.currentThread();
    this.maxDepth = maxDepth;
  }

  Polyglot polyglot() {
    return polyglot;
  }

  /** The current nesting depth (0: a top-level request). */
  int depth() {
    return depth;
  }

  void ready(int status, String info) {
    w.reset();
    w.u32(Wire.VERSION);
    w.str(System.getProperty("mle.class", "jvm"));
    w.u32(1 + Polyglot.LANGUAGES.size());
    w.str("java");
    for (String l : Polyglot.LANGUAGES) w.str(l);
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
    if (depth == 0) {
      seq = m.callSeq();
      FrameLimit.reset();
    }
    try {
      switch (m.op()) {
        case Wire.PREPARE -> prepare(m);
        case Wire.CALL -> call(m);
        case Wire.DROP -> {
          MessageReader r = new MessageReader(m.body());
          long id = r.u64();
          r.finish();
          java.drop(id);
          polyglot.drop(id);
          result(Wire.OK, null, null, null);
        }
        case Wire.RESET -> {
          java.reset();
          polyglot.reset();
          // A reset worker is usually parked for reuse: give its memory back (12 §17).
          System.gc();
          result(Wire.OK, null, null, null);
        }
        case Wire.OPTIONS -> options(m);
        case Wire.PARSE_LIBRARY -> parseLibrary(m);
        default -> result(Wire.INTERNAL, null, null, internal("unexpected message " + m.op()));
      }
    } finally {
      depth = savedDepth;
      seq = savedSeq;
    }
  }

  private void options(Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    int integerType = r.u8();
    int decimalType = r.u8();
    String zoneText = r.str();
    r.finish();
    java.time.ZoneId zone = Options.parseZone(zoneText);
    if (zone == null) {
      result(Wire.GUEST_ERROR, null, null, guest(Wire.LANGUAGE_TIME_ZONE, "unknown time zone '" + zoneText + "'"));
      return;
    }
    options.integerType = integerType;
    options.decimalType = decimalType;
    options.zone = zone;
    polyglot.applyOptions();
    result(Wire.OK, null, null, null);
  }

  /** CREATE LIBRARY: a syntax check; nothing runs. */
  private void parseLibrary(Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    String language = r.str();
    String name = r.str();
    String body = r.str();
    r.finish();
    String error;
    try {
      error = language.equals("java") ? java.checkLibrary(name, body) : polyglot.parseLibrary(language, name, body);
    } catch (Throwable t) {
      failed(t, false);
      return;
    }
    if (error == null) result(Wire.OK, null, null, null);
    else result(Wire.GUEST_ERROR, null, null, guest(language, error));
  }

  /** A database library's body from the host (Phase 6); see LibraryFileSystem. */
  LibraryFileSystem.Reply libraryRequest(String schema, String name, long knownHash) {
    if (Thread.currentThread() != thread)
      throw new IllegalStateException("libraries must be loaded on the routine's thread");
    w.reset();
    w.str(schema);
    w.str(name);
    w.u64(knownHash);
    mb.send(Wire.LIBRARY, Wire.OK, depth, seq, w);
    Mailbox.Message m = mb.receive();
    if (m.op() != Wire.LIBRARY_RESULT || m.depth() != depth)
      throw new IllegalStateException("unexpected message " + m.op() + " while loading a library");
    MessageReader r = new MessageReader(m.body());
    int status = r.u8();
    String language = r.str();
    String body = r.str();
    long hash = r.u64();
    String message = r.str();
    r.finish();
    return new LibraryFileSystem.Reply(status, language, body, hash, message);
  }

  private void prepare(Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    long id = r.u64();
    String language = r.str();
    String name = r.str();
    String entry = r.str();
    String source = r.str();
    int n = r.count();
    List<JavaRoutines.Param> params = new ArrayList<>(n);
    for (int i = 0; i < n; i++) params.add(new JavaRoutines.Param(r.str(), r.u8(), Values.readParamType(r)));
    Values.ParamType ret = r.flag() ? Values.readParamType(r) : null;
    int ni = r.count();
    List<LibraryFileSystem.Import> imports = new ArrayList<>(ni);
    for (int i = 0; i < ni; i++) imports.add(new LibraryFileSystem.Import(r.str(), r.str(), r.str()));
    r.finish();

    if (!language.equals("java") && !Polyglot.isGuest(language)) {
      result(Wire.GUEST_ERROR, null, null, guest(language, "the language " + language + " is not available in this worker"));
      return;
    }
    mb.stats(Wire.STATE_COMPILING, tainted);
    // A routine is (re)loaded: library DDL may have happened (see LibraryFileSystem).
    LibraryFileSystem.revalidate();
    // Loading runs routine code too (Python module level, JS top level), so PREPARE can be
    // interrupted like a call.
    boolean top = depth == 0;
    if (top) control.begin(seq, Polyglot.isGuest(language));
    String compileError = null;
    Throwable failure = null;
    try {
      // A routine id is prepared again after ALTER; it may change language.
      java.drop(id);
      polyglot.drop(id);
      if (language.equals("java")) java.prepare(id, name, entry, source, params, ret, imports);
      else polyglot.prepare(id, language, name, entry, source, params, ret, imports);
    } catch (JavaRoutines.CompileException e) {
      compileError = e.getMessage();
    } catch (Throwable t) {
      failure = t;
    }
    boolean interrupted = top && control.end();
    if (top) polyglot.settleInterrupt(control.takeInterrupter());
    if (failure != null) failed(failure, interrupted);
    else if (compileError != null) result(Wire.GUEST_ERROR, null, null, guest(language, compileError));
    else result(Wire.OK, null, null, null);
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
    Polyglot.Routine guestRoutine = routine == null ? polyglot.get(id) : null;
    int arity = routine != null ? routine.converters().length : guestRoutine != null ? guestRoutine.arity() : -1;
    if (arity != n) {
      result(Wire.INTERNAL, null, null, internal("routine " + id + " is not prepared with " + n + " parameters"));
      return;
    }
    Values.ParamType retType = routine != null ? routine.ret() : guestRoutine.ret();
    int[] outIndexes = routine != null ? routine.outs() : guestRoutine.outs();

    boolean top = depth == 0;
    if (top) control.begin(seq, guestRoutine != null);
    mb.stats(Wire.STATE_RUNNING, tainted);
    // The call's arena owns MemorySegment arguments; closing it makes them unusable afterwards.
    try (Arena callArena = Arena.ofConfined()) {
      Object ret = null;
      Object[] outs = new Object[n];
      Throwable failure = null;
      try {
        ret = routine != null ? java.invoke(routine, args, callArena, outs) : polyglot.invoke(guestRoutine, args, outs);
      } catch (Throwable t) {
        failure = t;
      }
      boolean interrupted = top && control.end();
      if (top) polyglot.settleInterrupt(control.takeInterrupter());
      // A killed statement reports the KILL, even if the routine then ended normally.
      if (failure == null && interrupted) result(Wire.INTERRUPTED, null, null, guest("", "interrupted"));
      else if (failure == null) result(Wire.OK, ret, retType, null, outIndexes, outs, paramTypes(routine, guestRoutine));
      else failed(failure, interrupted);
    }
  }

  private static Values.ParamType[] paramTypes(JavaRoutines.Routine j, Polyglot.Routine g) {
    return j != null ? j.types() : g.params();
  }

  private void failed(Throwable t, boolean interrupted) {
    if (t instanceof PolyglotException pe) {
      failedGuest(pe, interrupted);
      return;
    }
    if (interrupted && !(t instanceof OutOfMemoryError)) {
      result(Wire.INTERRUPTED, null, null, guest("java", "interrupted"));
      return;
    }
    if (t instanceof OutOfMemoryError || t instanceof StackOverflowError) {
      tainted = true;  // the host retires this worker after the call
      result(Wire.EXHAUSTED, null, null, new ErrorInfo("java", t.toString(), userFrames(t), 0, ""));
    } else if (interrupted) {
      result(Wire.INTERRUPTED, null, null, guest("java", "interrupted"));
    } else if (t instanceof MleSqlException e) {
      result(Wire.GUEST_ERROR, null, null,
          new ErrorInfo("java", e.getMessage(), userFrames(t), e.getErrorCode(), e.getSqlState()));
    } else if (t instanceof Values.ConversionException) {
      result(Wire.GUEST_ERROR, null, null, new ErrorInfo(Wire.LANGUAGE_TYPE, t.getMessage(), userFrames(t), 0, ""));
    } else {
      result(Wire.GUEST_ERROR, null, null, new ErrorInfo("java", describe(t), userFrames(t), 0, ""));
    }
  }

  /** Errors from JS and Python (or from Java code they called). */
  private void failedGuest(PolyglotException e, boolean interrupted) {
    String language = Polyglot.languageOf(e);
    List<Frame> frames = Polyglot.frames(e);
    if (e.isInterrupted() || e.isCancelled()) {
      result(Wire.INTERRUPTED, null, null, new ErrorInfo(language, "interrupted", frames, 0, ""));
      return;
    }
    // GraalPy turns an exhausted Java heap into a Python MemoryError; treat it as exhaustion (the
    // worker is retired), also when numpy merely refused a too large array: better safe.
    if (e.isResourceExhausted() || (language.equals("python") && e.isGuestException()
        && String.valueOf(e.getMessage()).startsWith("MemoryError"))) {
      tainted = true;
      result(Wire.EXHAUSTED, null, null, new ErrorInfo(language, Polyglot.describe(e, false), frames, 0, ""));
      return;
    }
    if (e.isHostException()) {
      Throwable h = e.asHostException();
      if (h instanceof OutOfMemoryError || h instanceof StackOverflowError) {
        tainted = true;
        result(Wire.EXHAUSTED, null, null, new ErrorInfo(language, h.toString(), frames, 0, ""));
        return;
      }
      if (h instanceof MleSqlException sql) {
        result(Wire.GUEST_ERROR, null, null,
            new ErrorInfo(language, sql.getMessage(), frames, sql.getErrorCode(), sql.getSqlState()));
        return;
      }
      if (h instanceof Values.ConversionException c) {
        result(Wire.GUEST_ERROR, null, null, new ErrorInfo(Wire.LANGUAGE_TYPE, c.getMessage(), frames, 0, ""));
        return;
      }
    }
    if (interrupted) {
      result(Wire.INTERRUPTED, null, null, new ErrorInfo(language, "interrupted", frames, 0, ""));
      return;
    }
    ErrorInfo sql = e.isGuestException() ? sqlError(language, e.getGuestObject(), frames) : null;
    if (sql != null) {
      result(Wire.GUEST_ERROR, null, null, sql);
      return;
    }
    // A Java error inside a language runtime (e.g. a class that failed to initialise): the
    // causes say why.
    String message = e.isHostException() && e.asHostException().getCause() != null
        ? describe(e.asHostException()) : Polyglot.describe(e, false);
    if (e.isInternalError()) {
      // A bug of a language runtime: the beginning of its trace helps to find it.
      java.io.StringWriter trace = new java.io.StringWriter();
      e.printStackTrace(new java.io.PrintWriter(trace));
      String t = trace.toString();
      message += " (internal error; " + t.substring(0, Math.min(t.length(), 1500)).replace('\n', ' ') + ")";
    }
    result(Wire.GUEST_ERROR, null, null, new ErrorInfo(language, message, frames, 0, ""));
  }

  /**
   * A guest SqlError (the SQL API's error class of JS and Python: marked with $mle$sql, with code
   * and sqlState): an inner statement's error, or one the routine throws (raised like SIGNAL).
   */
  private static ErrorInfo sqlError(String language, org.graalvm.polyglot.Value g, List<Frame> frames) {
    try {
      if (g == null || !g.hasMembers()) return null;
      org.graalvm.polyglot.Value mark = g.getMember("$mle$sql");
      if (mark == null || !mark.isBoolean() || !mark.asBoolean()) return null;
      org.graalvm.polyglot.Value code = g.getMember("code");
      org.graalvm.polyglot.Value state = g.getMember("sqlState");
      org.graalvm.polyglot.Value message = g.getMember("message");
      String text = message == null || message.isNull() ? "" : message.isString() ? message.asString()
          : message.toString();
      return new ErrorInfo(language, text, frames,
          code != null && code.fitsInInt() ? code.asInt() : 0,
          state != null && state.isString() ? state.asString() : "");
    } catch (RuntimeException e) {
      return null;
    }
  }

  private static String describe(Throwable t) {
    StringBuilder s = new StringBuilder(t.toString());
    for (Throwable c = t.getCause(); c != null && c != t; c = c.getCause()) s.append("; caused by ").append(c);
    return s.toString();
  }

  /** The routine's own frames (classes from routine loaders). */
  private static List<Frame> userFrames(Throwable t) {
    List<Frame> frames = new ArrayList<>();
    for (StackTraceElement e : t.getStackTrace()) {
      String loader = e.getClassLoaderName();
      if (loader != null && loader.startsWith(JavaRoutines.LOADER_PREFIX)) {
        JavaRoutines.Origin o = JavaRoutines.origins.get(loader);
        String function = e.getClassName() + "." + e.getMethodName();
        String file = e.getFileName() == null ? "" : e.getFileName();
        if (o != null && o.bodyForm()) {
          // The body's own frames: named like the routine (the wrapper class is ours).
          function = e.getMethodName().equals("call") ? "" : e.getMethodName();
          file = o.name();
        }
        frames.add(new Frame(function, file, Math.max(0, e.getLineNumber()), 0, 0));
      }
      if (frames.size() == Wire.MAX_STACK_FRAMES) break;
    }
    return frames;
  }

  private static ErrorInfo guest(String language, String message) {
    return new ErrorInfo(language, message, List.of(), 0, "");
  }

  private static ErrorInfo internal(String message) {
    return new ErrorInfo("", message, List.of(), 0, "");
  }

  private void result(int status, Object ret, Values.ParamType retType, ErrorInfo error) {
    result(status, ret, retType, error, null, null, null);
  }

  /**
   * Posts a RESULT with the OUT values outs[i] for i in outIndexes. A value that cannot be
   * converted turns into a conversion error.
   */
  private void result(int status, Object ret, Values.ParamType retType, ErrorInfo error, int[] outIndexes,
      Object[] outs, Values.ParamType[] types) {
    try {
      writeResult(ret, retType, error, outIndexes, outs, types);
    } catch (Values.ConversionException e) {
      status = Wire.GUEST_ERROR;
      writeResult(null, null, guest(Wire.LANGUAGE_TYPE, e.getMessage()), null, null, null);
    }
    mb.stats(depth == 0 ? Wire.STATE_IDLE : Wire.STATE_RUNNING, tainted);
    mb.send(Wire.RESULT, status, depth, seq, w);
  }

  private void writeResult(Object ret, Values.ParamType retType, ErrorInfo error, int[] outIndexes, Object[] outs,
      Values.ParamType[] types) {
    w.reset();
    if (retType != null && error == null) {
      w.u8(1);
      Values.writeAs(w, ret, retType);
    } else {
      w.u8(0);
    }
    if (error == null && outIndexes != null) {
      w.u32(outIndexes.length);
      for (int i : outIndexes) {
        w.u32(i);
        Values.writeAs(w, outs[i], types[i]);
      }
    } else {
      w.u32(0);
    }
    w.bytes(console.takeOut());
    w.bytes(console.takeErr());
    w.u8(console.takeCleared() ? 1 : 0);
    if (error == null) {
      w.u8(0);
      return;
    }
    w.u8(1);
    w.str(error.language());
    w.str(error.message());
    w.u32(error.frames().size());
    for (Frame f : error.frames()) {
      w.str(f.function());
      w.str(f.file());
      w.u32(f.line());
      w.u32(f.column());
      w.u32(f.endColumn());
    }
    w.u32(error.sqlErrno());
    w.str(error.sqlState() == null ? "" : error.sqlState());
  }

  // ------------------------------------------------------------ SQL

  private long nextStatementId;

  /** A new statement id for this worker (unique within any call). */
  long newStatementId() {
    return ++nextStatementId;
  }

  /**
   * One SQL request to the host (Phase 5 protocol: action, statement id, text, parameters,
   * flags); throws MleSqlException for an SQL error.
   */
  MleSql.Reply sqlRequest(int action, long id, String text, Object[] params, int flags, int maxRows) {
    return sqlRequest(action, id, text, params, flags, maxRows, false);
  }

  /**
   * raw: row values stay as read (DATE/TIME/DATETIME/TIMESTAMP as Values.Temporal, VECTOR as
   * float[]) for the language mappers of com.percona.mle.sql.
   */
  MleSql.Reply sqlRequest(int action, long id, String text, Object[] params, int flags, int maxRows,
      boolean raw) {
    if (Thread.currentThread() != thread)
      throw new IllegalStateException("SQL must be run on the routine's thread");
    w.reset();
    w.u8(action);
    w.u64(id);
    w.str(text == null ? "" : text);
    w.u32(params == null ? 0 : params.length);
    if (params != null) for (Object p : params) Values.writeParam(w, p);
    w.u8(flags);
    w.u32(maxRows);
    mb.send(Wire.SQL, Wire.OK, depth, seq, w);
    while (true) {
      Mailbox.Message m = mb.receive();
      if (m.op() == Wire.SQL_RESULT && m.depth() == depth) return sqlReply(id, new MessageReader(m.body()), raw);
      boolean nested = m.op() == Wire.CALL || m.op() == Wire.PREPARE || m.op() == Wire.DROP;
      if (nested && m.depth() == depth + 1 && m.depth() <= maxDepth) {
        handle(m);
        continue;
      }
      throw new IllegalStateException("unexpected message " + m.op() + " at depth " + m.depth() + " during SQL");
    }
  }

  /** The top-level call's sequence number (per-call state of the SQL API). */
  long callSeq() {
    return seq;
  }

  /** Whether the running top-level call was killed (mysql.sleep). */
  boolean interrupted() {
    return control.isInterrupted();
  }

  private static MleSql.Reply sqlReply(long id, MessageReader r, boolean raw) {
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
        List<MleSql.Warning> warnings = warnings(r);
        boolean moreResults = r.flag();
        int paramCount = (int) r.u32();
        long execUs = r.u64();
        r.finish();
        return new MleSql.Reply(id, false, List.of(), List.of(), false, moreResults, affected, insertId, warnings,
            paramCount, execUs);
      }
      case Wire.SQL_ROWS -> {
        int ncols = r.count();
        List<MleSql.Column> columns = new ArrayList<>(ncols);
        for (int i = 0; i < ncols; i++) {
          String name = r.str();
          String table = r.str();
          Values.ParamType t = Values.readParamType(r);
          columns.add(new MleSql.Column(name, table, t.sqlName(), t.unsigned(), t.length(), (int) t.scale(),
              r.str(), r.str(), r.str(), r.str(), r.str(), (int) r.u32()));
        }
        long nrows = r.u32();
        List<Object[]> rows = new ArrayList<>((int) Math.min(nrows, 1 << 16));
        for (long i = 0; i < nrows; i++) {
          Object[] row = new Object[ncols];
          for (int c = 0; c < ncols; c++) {
            Object v = Values.read(r);
            row[c] = !raw ? Values.toJava(v) : v instanceof Values.VectorRef ref ? ref.toArray() : v;
          }
          rows.add(row);
        }
        boolean more = r.flag();
        boolean moreResults = r.flag();
        List<MleSql.Warning> warnings = warnings(r);
        long execUs = r.u64();
        r.finish();
        return new MleSql.Reply(id, true, List.copyOf(columns), rows, more, moreResults, 0, 0, warnings, 0,
            execUs);
      }
      default -> throw new IllegalStateException("bad SQL result kind " + kind);
    }
  }

  private static List<MleSql.Warning> warnings(MessageReader r) {
    int n = r.count();
    List<MleSql.Warning> list = new ArrayList<>(n);
    for (int i = 0; i < n; i++) list.add(new MleSql.Warning((int) r.u32(), (int) r.u32(), r.str()));
    return list;
  }

}
