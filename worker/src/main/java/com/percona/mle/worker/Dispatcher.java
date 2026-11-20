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
import com.percona.mle.sql.RowSink;
import com.percona.mle.sql.Statements;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.graalvm.polyglot.PolyglotException;

/**
 * Serves the host's requests (PREPARE, CALL, PARSE_LIBRARY, DROP, RESET, OPTIONS) on the worker's
 * main thread and answers each with exactly one RESULT (§9); sends the SQL and LIBRARY messages
 * of the guest code it runs, serving nested requests while it waits for an SQL_RESULT (§10.6).
 *
 * <p>Nesting: a routine's SQL may call another routine of this worker. The host then sends CALL
 * (or PREPARE, DROP) with depth + 1 while the worker waits for SQL_RESULT; it is handled
 * recursively. Each request has a {@link Request}: its routine, its library epoch, its statement
 * ids and their states. Only the guest code of the innermost request, on this thread, may send
 * SQL and LIBRARY; anything else gets an error in guest code and nothing is posted.
 *
 * <p>A message of the host that is malformed or out of place ends the process (ProtocolFailure):
 * it is never an exception that guest code could see.
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
  /** The languages of READY: what PREPARE and PARSE_LIBRARY may name, and an Error's language. */
  private final List<String> languages;
  /** The request being served at the innermost depth, or null between requests. */
  private Request current;
  /** Numbers the requests of this worker, nested ones too, and the ranges of their statement ids. */
  private long serials;
  /** The call_seq of the last top-level request: the next one's is 1 more (§8.2). */
  private long lastSeq;
  /** Options are in force from an OPTIONS answered OK until RESET (§9.8). */
  private boolean optionsInForce;
  private boolean tainted;
  /** RESET asked for a GC (memory back before the worker waits); run when idle, not on the way. */
  private boolean gcWhenIdle;
  /** How long a reset worker waits for a request before it collects (exp note 38). */
  private static final long IDLE_GC_NS = 200_000_000L;
  private final ThreadWatch threads = new ThreadWatch();
  private static final byte[] NO_BYTES = new byte[0];

  /** text: the frame's source line (trimmed, at most 200 characters), for tracebacks. */
  record Frame(String function, String file, int line, int column, int endColumn, String text) {
    static String line(String s) {
      if (s == null) return "";
      s = s.strip();
      return s.length() <= 200 ? s : s.substring(0, 200) + "...";
    }
  }

  /** An Error of §7.4. sqlErrorId: the error_id of the SQL_RESULT ERROR the error came from, or 0. */
  record ErrorInfo(String language, String message, List<Frame> frames, int sqlErrno, String sqlState,
      long sqlErrorId) {}

  /**
   * One request being served. Statement ids are scope << 24 | n: no two requests share a scope,
   * so an id says whose it is, and a request that needs more ids takes another scope.
   */
  private final class Request implements Mailbox.Admission {
    static final int ID_BITS = 24;
    final int op;
    final int depth;
    final long seq;
    final long scope;
    /** PREPARE and CALL: the routine, the library epoch of the request, the routine's schema. */
    long routine;
    long epoch;
    String schema;
    /** Guest code of this request may run now: it may send SQL and LIBRARY. */
    boolean activation;
    boolean answered;
    Statements statements;
    private long idScope;
    private long ids;
    private List<Long> moreScopes;
    /** A result whose unread rows are drained before another statement of this request runs. */
    Runnable deferred;
    long deferredId;

    Request(int op, int depth, long seq) {
      this.op = op;
      this.depth = depth;
      this.seq = seq;
      this.scope = ++serials;
      this.idScope = scope;
    }

    Statements statements() {
      if (statements == null) statements = new Statements();
      return statements;
    }

    long newStatementId() {
      if (ids == (1L << ID_BITS) - 1) {
        if (moreScopes == null) moreScopes = new ArrayList<>();
        moreScopes.add(idScope);
        idScope = ++serials;
        ids = 0;
      }
      return idScope << ID_BITS | ++ids;
    }

    boolean owns(long id) {
      long s = id >>> ID_BITS;
      return s == idScope || (moreScopes != null && moreScopes.contains(s));
    }

    /**
     * While this request waits for an SQL_RESULT (§8.1): the reply, or a request nested in the
     * statement, which only a CALL's statement can have.
     */
    @Override
    public void check(int o, int d, long s) {
      boolean reply = o == Wire.SQL_RESULT && d == depth;
      boolean nested = op == Wire.CALL && (o == Wire.PREPARE || o == Wire.CALL || o == Wire.DROP) && d == depth + 1
          && d <= Wire.MAX_DEPTH;
      if (s != seq || !(reply || nested))
        throw ProtocolFailure.exit("op " + o + " at depth " + d + " of request " + Long.toUnsignedString(s)
            + " while request " + Long.toUnsignedString(seq) + " waits for SQL_RESULT at depth " + depth);
    }

    /** While this request waits for a LIBRARY_RESULT: nothing else. */
    void checkLibraryResult(int o, int d, long s) {
      if (o != Wire.LIBRARY_RESULT || d != depth || s != seq)
        throw ProtocolFailure.exit("op " + o + " at depth " + d + " while request " + Long.toUnsignedString(seq)
            + " waits for LIBRARY_RESULT at depth " + depth);
    }
  }

  Dispatcher(Mailbox mb, JavaRoutines java, Polyglot polyglot, Console console, Control control, Options options) {
    this.mb = mb;
    this.w = new MessageWriter(mb.arena);
    this.java = java;
    this.polyglot = polyglot;
    this.console = console;
    this.control = control;
    this.options = options;
    this.thread = Thread.currentThread();
    List<String> l = new ArrayList<>();
    l.add("java");
    l.addAll(Polyglot.LANGUAGES);
    this.languages = List.copyOf(l);
  }

  Polyglot polyglot() {
    return polyglot;
  }

  /** Each prepared routine's schema (the current database while it runs): exp note 52's engine cache. */
  private final java.util.Map<Long, String> schemas = new java.util.HashMap<>();

  /** The schema of the routine whose request is served, or null. */
  String routineSchema() {
    Request q = current;
    return q == null ? null : q.schema;
  }

  /** The current nesting depth (0: a top-level request). */
  int depth() {
    Request q = current;
    return q == null ? 0 : q.depth;
  }

  /**
   * The request whose guest code may send SQL and LIBRARY now (§10.1): the one served at the
   * innermost depth, on this thread, while its guest code runs. Anything else (another thread, code
   * that runs between requests or while a request without guest code is served, a row sink) gets
   * an error, and nothing is posted.
   */
  private Request serving(String what) {
    if (Thread.currentThread() != thread)
      throw new IllegalStateException(what + " only on the thread that runs the routine");
    Request q = current;
    if (q == null || !q.activation)
      throw new IllegalStateException(what + " only by a routine while it is called");
    // The reply being read is in the mailbox, where a request would be written over it.
    if (decodingRows) throw new IllegalStateException(what + " not while a result is being read (a RowSink)");
    return q;
  }

  /** The worker_class of READY: the host's name for this kind of worker, if it has the form of one. */
  private static String workerClass() {
    String c = System.getProperty("mle.class", "jvm");
    return Text.identifier(c) ? c : "jvm";
  }

  /** Posts READY (§9.1): one chunk; with a status other than OK, info is why the worker cannot serve. */
  void ready(int status, String info) {
    w.reset(Wire.MAX_SMALL_MESSAGE);
    w.u32(Wire.VERSION);
    w.str(Text.cut(info, Wire.MAX_ERROR_MESSAGE));
    w.u64(0);  // features: version 1.0 defines none
    w.str(workerClass());
    w.u32(languages.size());
    for (String l : languages) w.str(l);
    w.str(Text.cut("Java " + Runtime.version() + " (" + System.getProperty("java.vm.name") + ")", Wire.MAX_FRAME_TEXT));
    mb.stats(Wire.STATE_IDLE, false);
    mb.send(Wire.READY, status, 0, 0, w);
  }

  void loop() {
    while (true) {
      // After a RESET: the full GC gives memory back to a worker that now waits for a session.
      // Done only if no request comes for a while, so a session that takes the worker at once
      // (back to back, the same account) does not wait ~20 ms for it (exp note 38).
      if (gcWhenIdle) {
        gcWhenIdle = false;
        if (!mb.awaitPending(IDLE_GC_NS)) {
          System.gc();
          mb.stats(Wire.STATE_IDLE, tainted);  // the host's memory figures: after the GC
        }
      }
      Mailbox.Message m = mb.receive(this::checkTopLevel);
      lastSeq = m.callSeq();
      handle(m);
    }
  }

  /** While the worker is idle (§8.1): a top-level request with the next call_seq. */
  private void checkTopLevel(int op, int depth, long seq) {
    boolean needsOptions = op == Wire.PREPARE || op == Wire.CALL || op == Wire.PARSE_LIBRARY;
    if (!(needsOptions || op == Wire.DROP || op == Wire.RESET || op == Wire.OPTIONS) || depth != 0
        || seq != lastSeq + 1)
      throw ProtocolFailure.exit("op " + op + " at depth " + depth + " with call_seq " + Long.toUnsignedString(seq)
          + " while idle after request " + Long.toUnsignedString(lastSeq));
    if (needsOptions && !optionsInForce) throw ProtocolFailure.exit("op " + op + " without options in force");
  }

  /** The worker itself failed where it cannot answer: its state towards the host is not known. */
  private Error fatal(String where, Throwable t) {
    try {
      console.log().println("mle worker: fatal: " + where + ": " + t);
      t.printStackTrace(console.log());
    } catch (Throwable ignored) {
      // the reason is for the log only
    }
    Runtime.getRuntime().halt(1);
    throw new InternalError(where);
  }

  /** Serves one request and posts its RESULT. Nothing is thrown: guest code of an outer request runs above. */
  private void handle(Mailbox.Message m) {
    Request outer = current;
    Request q = new Request(m.op(), m.depth(), m.callSeq());
    current = q;
    boolean top = q.depth == 0;
    if (top) {
      FrameLimit.reset();
      LibraryFileSystem.requestStart();
      sqlErrors.clear();
    }
    try {
      switch (q.op) {
        case Wire.PREPARE -> prepare(q, m);
        case Wire.CALL -> call(q, m);
        case Wire.PARSE_LIBRARY -> parseLibrary(q, m);
        case Wire.DROP -> drop(q, m);
        case Wire.RESET -> reset(q, m);
        default -> options(q, m);
      }
    } catch (Throwable t) {
      // The worker's own failure: INTERNAL, and the worker is not to be used again.
      if (q.answered) throw fatal("after the RESULT of op " + q.op, t);
      tainted = true;
      try {
        q.activation = false;
        if (top) {
          control.end();
          polyglot.settleInterrupt(control.takeInterrupter());
        }
        result(q, Wire.INTERNAL, internal("the worker failed: " + t));
      } catch (Throwable t2) {
        throw fatal("answering op " + q.op, t2);
      }
    } finally {
      current = outer;
    }
  }

  private void unprepare(long id) {
    java.drop(id);
    polyglot.drop(id);
    schemas.remove(id);
  }

  private void drop(Request q, Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    long id = r.u64();
    r.finish();
    if (q.depth == 0) control.begin(q.seq, false, false);
    unprepare(id);
    LibraryFileSystem.discard(id);
    result(q, Wire.OK, null);
  }

  private void reset(Request q, Mailbox.Message m) {
    new MessageReader(m.body()).finish();
    control.begin(q.seq, false, false);
    java.reset();
    polyglot.reset();
    schemas.clear();
    LibraryFileSystem.reset();
    // What guest code printed and no RESULT carried is dropped; the options end.
    console.discard();
    options.reset();
    polyglot.applyOptions();
    optionsInForce = false;
    // A worker with a thread a routine left is not kept for reuse (the host retires it).
    if (!tainted && threads.strangers()) tainted = true;
    // The host took the memory watcher's count with the session's statistics.
    MemoryWatch.clear();
    // A reset worker is usually parked for reuse: give its memory back (12 §17), once idle.
    gcWhenIdle = true;
    result(q, Wire.OK, null);
  }

  private static boolean settingName(String s) {
    if (s.isEmpty() || s.length() > Wire.MAX_SETTING_NAME) return false;
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '.') return false;
    }
    return true;
  }

  /**
   * OPTIONS (§9.8): the complete set, so a setting that is absent has its default. Names this
   * worker does not know are ignored. A zone it does not know or a value it does not accept
   * leaves the options in force as they were.
   */
  private void options(Request q, Mailbox.Message m) {
    MessageReader r = new MessageReader(m.body());
    String zoneText = r.str();
    int zoneBytes = zoneText.getBytes(StandardCharsets.UTF_8).length;
    if (zoneBytes < 1 || zoneBytes > Wire.MAX_TIME_ZONE) throw ProtocolFailure.exit("a time_zone of " + zoneBytes + " bytes");
    int n = r.count(8, Wire.MAX_SETTINGS);
    int integerType = Options.INT_UNSAFE_STRING;
    int decimalType = Options.DECIMAL_STRING;
    String refused = null;
    String previous = null;
    for (int i = 0; i < n; i++) {
      String name = r.str();
      String value = r.str();
      if (!settingName(name) || (previous != null && previous.compareTo(name) >= 0)
          || value.getBytes(StandardCharsets.UTF_8).length > Wire.MAX_SETTING_VALUE)
        throw ProtocolFailure.exit("bad setting '" + name + "' in OPTIONS");
      previous = name;
      int v;
      if (name.equals(Options.INTEGER_TYPE)) {
        if ((v = Options.integerType(value)) >= 0) integerType = v;
        else if (refused == null) refused = name;
      } else if (name.equals(Options.DECIMAL_TYPE)) {
        if ((v = Options.decimalType(value)) >= 0) decimalType = v;
        else if (refused == null) refused = name;
      }
    }
    r.finish();
    control.begin(q.seq, false, false);
    java.time.ZoneId zone = Options.parseZone(zoneText);
    if (zone == null) {
      result(q, Wire.GUEST_ERROR, guest(Wire.LANGUAGE_TIME_ZONE, "unknown time zone '" + zoneText + "'"));
      return;
    }
    if (refused != null) {
      result(q, Wire.GUEST_ERROR, guest(Wire.LANGUAGE_OPTION, "the value of the setting " + refused + " is not one of its values"));
      return;
    }
    options.integerType = integerType;
    options.decimalType = decimalType;
    options.zone = zone;
    polyglot.applyOptions();
    optionsInForce = true;
    result(q, Wire.OK, null);
  }

  /** The Error of a host message the worker has no memory for (§5.3). */
  private static ErrorInfo cannotHold(String what) {
    return internal(what + " is larger than this worker can hold");
  }

  /** The interruptible top-level request is over: the worker's threads are looked at. */
  private void afterTopLevel() {
    polyglot.settleInterrupt(control.takeInterrupter());
    if (!tainted && threads.strangersNow()) tainted = true;  // retired, not kept
  }

  /** CREATE LIBRARY: a syntax check; nothing runs. */
  private void parseLibrary(Request q, Mailbox.Message m) {
    String language = null, name = null, body = null;
    if (!m.discarded()) {
      try {
        MessageReader r = new MessageReader(m.body());
        language = r.str();
        name = r.str();
        body = r.str();
        r.finish();
      } catch (OutOfMemoryError e) {
        body = null;
      }
    }
    if (body == null) {
      control.begin(q.seq, false, false);
      result(q, Wire.EXHAUSTED, cannotHold("the library"));
      return;
    }
    if (!languages.contains(language)) throw ProtocolFailure.exit("PARSE_LIBRARY in the language '" + language + "'");
    control.begin(q.seq, true, !language.equals("java"));
    mb.stats(Wire.STATE_COMPILING, tainted);
    String error = null;
    Throwable failure = null;
    try {
      error = !language.equals("java") ? polyglot.parseLibrary(language, name, body)
          : java.checkLibrary(name, body);
    } catch (Throwable t) {
      failure = t;
    }
    boolean stopped = control.end();
    afterTopLevel();
    if (failure != null) failed(q, failure, stopped);
    else if (stopped) result(q, Wire.INTERRUPTED, guest("", "interrupted"));
    else if (error == null) result(q, Wire.OK, null);
    else result(q, Wire.GUEST_ERROR, guest(language, error));
  }

  /** mysqld's answer to a LIBRARY message (§9.10), for LibraryFileSystem. */
  LibraryFileSystem.Reply libraryRequest(String schema, String name, long knownVersion) {
    Request q = serving("a library can be imported");
    w.reset(Wire.MAX_SMALL_MESSAGE);
    w.str(schema);
    w.str(name);
    w.u64(knownVersion);
    mb.send(Wire.LIBRARY, Wire.OK, q.depth, q.seq, w);
    Mailbox.Message m;
    try {
      m = mb.receive(q::checkLibraryResult);
    } catch (Throwable t) {
      throw fatal("waiting for LIBRARY_RESULT", t);
    }
    // A body the worker has no memory for: as if the host had refused the import (§5.3).
    if (m.discarded())
      return new LibraryFileSystem.Reply(Wire.LIBRARY_REFUSED, "", "", 0, "the library is larger than this worker can hold");
    try {
      MessageReader r = new MessageReader(m.body());
      int status = r.u8();
      String language = r.str();
      String body = r.str();
      long version = r.u64();
      String message = r.str();
      r.finish();
      boolean ok = switch (status) {
        case Wire.LIBRARY_REFUSED -> language.isEmpty() && body.isEmpty() && version == 0;
        case Wire.LIBRARY_UNCHANGED -> knownVersion != 0 && version == knownVersion && Text.identifier(language)
            && body.isEmpty() && message.isEmpty();
        case Wire.LIBRARY_BODY -> version != 0 && Text.identifier(language) && message.isEmpty();
        default -> false;
      };
      if (!ok) throw ProtocolFailure.exit("a LIBRARY_RESULT with status " + status + " and fields that do not fit it");
      return new LibraryFileSystem.Reply(status, language, body, version, message);
    } catch (OutOfMemoryError e) {
      return new LibraryFileSystem.Reply(Wire.LIBRARY_REFUSED, "", "", 0, "the library is larger than this worker can hold");
    }
  }

  /** The routine id and library epoch of the request whose guest code runs now (§9.10). */
  long[] libraryScope() {
    Request q = serving("a library can be imported");
    return new long[] {q.routine, q.epoch};
  }

  /** The fields of a PREPARE after its routine_id. */
  private record Prepare(String language, String name, String entry, String source, List<JavaRoutines.Param> params,
      Values.ParamType ret, List<LibraryFileSystem.Import> imports, String schema, boolean check, long epoch) {
    static Prepare read(MessageReader r) {
      String language = r.str();
      String name = r.str();
      String entry = r.str();
      String source = r.str();
      int n = r.count(23, Wire.MAX_PARAMS);
      List<JavaRoutines.Param> params = new ArrayList<>(n);
      for (int i = 0; i < n; i++) {
        String param = r.str();
        int mode = r.u8();
        if (mode > Wire.MODE_INOUT) throw ProtocolFailure.exit("a Param with mode " + mode);
        params.add(new JavaRoutines.Param(param, mode, Values.readType(r)));
      }
      Values.ParamType ret = r.bool() ? Values.readType(r) : null;
      int ni = r.count(12, Wire.MAX_IMPORTS);
      List<LibraryFileSystem.Import> imports = new ArrayList<>(ni);
      for (int i = 0; i < ni; i++) imports.add(new LibraryFileSystem.Import(r.str(), r.str(), r.str()));
      String schema = r.str();
      boolean check = r.bool();
      long epoch = r.u64();
      r.finish();
      return new Prepare(language, name, entry, source, params, ret, imports, schema, check, epoch);
    }
  }

  private void prepare(Request q, Mailbox.Message m) {
    boolean top = q.depth == 0;
    // Of a message the worker could not hold, the first bytes are kept: the routine_id.
    MessageReader r = new MessageReader(m.body());
    long id = r.u64();
    q.routine = id;
    // The id is prepared only by a PREPARE that answers OK, whatever it was before (§8.3); the
    // answers of its libraries are discarded (§9.10). A running activation keeps its code.
    unprepare(id);
    LibraryFileSystem.discard(id);
    Prepare p = null;
    if (!m.discarded()) {
      try {
        p = Prepare.read(r);
      } catch (OutOfMemoryError e) {
        p = null;
      }
    }
    if (p == null) {
      if (top) control.begin(q.seq, false, false);
      result(q, Wire.EXHAUSTED, cannotHold("the routine"));
      return;
    }
    String language = p.language();
    if (!languages.contains(language)) throw ProtocolFailure.exit("PREPARE in the language '" + language + "'");
    boolean javaRoutine = language.equals("java");
    q.epoch = p.epoch();
    q.schema = p.schema();
    schemas.put(id, p.schema());
    // Loading runs routine code too (Python module level, JS top level), so PREPARE can be
    // interrupted like a call. A nested request that arrives after the interrupt runs nothing.
    if (top) {
      control.begin(q.seq, true, !javaRoutine);
    } else if (control.refuse()) {
      result(q, Wire.INTERRUPTED, guest("", "interrupted"));
      return;
    }
    mb.stats(Wire.STATE_COMPILING, tainted);
    String compileError = null;
    Throwable failure = null;
    q.activation = true;
    try {
      if (p.entry().equals("<eval>")) {  // mle_eval (exp note 39)
        if (javaRoutine) java.prepareEval(id, p.source());
        else polyglot.prepareEval(id, language, p.source(), p.ret());
      } else if (javaRoutine) {
        java.prepare(id, p.schema(), p.name(), p.entry(), p.source(), p.params(), p.ret(), p.imports());
      } else {
        polyglot.check = p.check();
        try {
          polyglot.prepare(id, language, p.name(), p.entry(), p.source(), p.params(), p.ret(), p.imports());
        } finally {
          polyglot.check = false;
        }
      }
    } catch (JavaRoutines.CompileException e) {
      compileError = e.getMessage();
    } catch (Throwable t) {
      failure = t;
    }
    q.activation = false;
    boolean stopped = top ? control.end() : control.stopped();
    if (top) afterTopLevel();
    List<String> findings = polyglot.takeFindings();
    if (failure != null) failed(q, failure, stopped);
    else if (stopped) result(q, Wire.INTERRUPTED, guest("", "interrupted"));
    else if (compileError != null) result(q, Wire.GUEST_ERROR, guest(language, compileError));
    else result(q, Wire.OK, null, null, null, null, null, null, p.check() ? findings : List.of());
  }

  private void call(Request q, Mailbox.Message m) {
    boolean top = q.depth == 0;
    JavaRoutines.Routine routine = null;
    Polyglot.Routine guestRoutine = null;
    Object[] args = null;
    if (!m.discarded()) {
      MessageReader r = new MessageReader(m.body());
      long id = r.u64();
      q.routine = id;
      q.epoch = r.u64();
      q.schema = schemas.get(id);
      int n = r.count(1, Wire.MAX_VALUES);
      // The host sends CALL only for a prepared id, with one argument per Param (§9.3).
      routine = java.get(id);
      guestRoutine = routine == null ? polyglot.get(id) : null;
      if (routine == null && guestRoutine == null)
        throw ProtocolFailure.exit("CALL of routine " + Long.toUnsignedString(id) + ", which is not prepared");
      Values.ParamType[] types = routine != null ? routine.types() : guestRoutine.params();
      if (types.length != n)
        throw ProtocolFailure.exit("CALL of routine " + Long.toUnsignedString(id) + " with " + n + " arguments for "
            + types.length + " parameters");
      try {
        Object[] a = new Object[n];
        for (int i = 0; i < n; i++) a[i] = Values.read(r, types[i].tag());
        r.finish();
        args = a;
      } catch (OutOfMemoryError e) {
        args = null;
      }
    }
    if (args == null) {
      if (top) control.begin(q.seq, false, false);
      result(q, Wire.EXHAUSTED, cannotHold("the call's arguments"));
      return;
    }
    if (top) {
      control.begin(q.seq, true, guestRoutine != null);
    } else if (control.refuse()) {
      result(q, Wire.INTERRUPTED, guest("", "interrupted"));
      return;
    }
    mb.stats(Wire.STATE_RUNNING, tainted);
    // The call's arena owns MemorySegment arguments; closing it makes them unusable afterwards.
    try (Arena callArena = Arena.ofConfined()) {
      Object ret = null;
      Object[] outs = new Object[args.length];
      Throwable failure = null;
      q.activation = true;
      try {
        if (routine != null) {
          // A Java routine is compiled with its Java libraries: linked anew if one has changed (§9.10).
          routine = java.linked(routine);
          ret = java.invoke(routine, args, callArena, outs);
        } else {
          ret = polyglot.invoke(guestRoutine, args, outs);
        }
      } catch (Throwable t) {
        failure = t;
      }
      q.activation = false;
      boolean stopped = top ? control.end() : control.stopped();
      if (top) afterTopLevel();
      // The routine was stopped: also when its code went on and returned (§11.1).
      if (failure != null) failed(q, failure, stopped);
      else if (stopped) result(q, Wire.INTERRUPTED, guest("", "interrupted"));
      else if (routine != null) result(q, Wire.OK, ret, routine.ret(), null, routine.outs(), outs, routine.types(), List.of());
      else result(q, Wire.OK, ret, guestRoutine.ret(), null, guestRoutine.outs(), outs, guestRoutine.params(), List.of());
    }
  }

  /**
   * The RESULT of a request that failed with t. stopped: the request's interrupt was delivered and
   * acted on; only then is the status INTERRUPTED (§10.6): what a runtime calls interrupted for
   * reasons of its own is an error of the guest code.
   */
  private void failed(Request q, Throwable t, boolean stopped) {
    if (t instanceof PolyglotException pe) {
      failedGuest(q, pe, stopped);
      return;
    }
    if (stopped && !(t instanceof OutOfMemoryError)) {
      result(q, Wire.INTERRUPTED, guest("java", "interrupted"));
      return;
    }
    if (t instanceof OutOfMemoryError || t instanceof StackOverflowError) {
      tainted = true;  // the host retires this worker after the top-level request
      result(q, Wire.EXHAUSTED, new ErrorInfo("java", t.toString(), userFrames(t), 0, "", 0));
    } else if (t instanceof MleSqlException e) {
      result(q, Wire.GUEST_ERROR, sqlInfo("java", e.getMessage(), userFrames(t), e.getErrorCode(), e.getSqlState(),
          e.getErrorId()));
    } else if (t instanceof Values.ConversionException) {
      result(q, Wire.GUEST_ERROR, new ErrorInfo(Wire.LANGUAGE_TYPE, t.getMessage(), userFrames(t), 0, "", 0));
    } else if (t instanceof JavaRoutines.CompileException) {
      result(q, Wire.GUEST_ERROR, guest("java", t.getMessage()));
    } else {
      result(q, Wire.GUEST_ERROR, new ErrorInfo("java", describe(t), userFrames(t), 0, "", 0));
    }
  }

  /** Python's MemoryError itself, not a class of the routine named MemoryErrorSomething. */
  private static boolean memoryError(String message) {
    return message != null && message.startsWith("MemoryError")
        && (message.length() == 11 || message.charAt(11) == ':');
  }

  /** Errors from JS and Python (or from Java code they called). */
  private void failedGuest(Request q, PolyglotException e, boolean stopped) {
    String language = Polyglot.languageOf(e);
    List<Frame> frames = Polyglot.frames(e);
    if (e.isInterrupted() || e.isCancelled()) {
      // Cancelled: the Context was closed under the call (a routine can reach it: Mle.context()),
      // and nothing runs in it again; the host retires this worker after the top-level request.
      if (e.isCancelled()) tainted = true;
      if (stopped) result(q, Wire.INTERRUPTED, new ErrorInfo(language, "interrupted", frames, 0, "", 0));
      // What the runtime calls cancelled because guest code provoked it is an error of guest
      // code (§10.6), like its own "interrupted" below; tainted retires the worker.
      else if (e.isCancelled())
        result(q, Wire.GUEST_ERROR, new ErrorInfo(language, "the worker's context was closed by a routine", frames, 0, "", 0));
      else result(q, Wire.GUEST_ERROR, new ErrorInfo(language, "interrupted by the routine's own code", frames, 0, "", 0));
      return;
    }
    // GraalPy turns an exhausted Java heap into a Python MemoryError; treat it as exhaustion (the
    // worker is retired), also when numpy merely refused a too large array: better safe.
    if (e.isResourceExhausted() || (language.equals("python") && e.isGuestException()
        && memoryError(e.getMessage()))) {
      tainted = true;
      result(q, Wire.EXHAUSTED, new ErrorInfo(language, Polyglot.describe(e, false), frames, 0, "", 0));
      return;
    }
    if (e.isHostException()) {
      Throwable h = e.asHostException();
      if (h instanceof OutOfMemoryError || h instanceof StackOverflowError) {
        tainted = true;
        result(q, Wire.EXHAUSTED, new ErrorInfo(language, h.toString(), frames, 0, "", 0));
        return;
      }
      if (h instanceof MleSqlException sql) {
        result(q, Wire.GUEST_ERROR, sqlInfo(language, sql.getMessage(), frames, sql.getErrorCode(), sql.getSqlState(),
            sql.getErrorId()));
        return;
      }
      if (h instanceof Values.ConversionException c) {
        result(q, Wire.GUEST_ERROR, new ErrorInfo(Wire.LANGUAGE_TYPE, c.getMessage(), frames, 0, "", 0));
        return;
      }
    }
    if (stopped) {
      result(q, Wire.INTERRUPTED, new ErrorInfo(language, "interrupted", frames, 0, "", 0));
      return;
    }
    ErrorInfo sql = e.isGuestException() ? sqlError(language, e.getGuestObject(), frames) : null;
    if (sql != null) {
      result(q, Wire.GUEST_ERROR, sql);
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
    result(q, Wire.GUEST_ERROR, new ErrorInfo(language, message, frames, 0, "", 0));
  }

  /**
   * The ERRORs of SQL_RESULTs of the top-level request by their error_id (§10.4), the most recent
   * ones: a routine that ends with one of them hands it back by its id (§10.8).
   */
  private final java.util.Map<Long, MleSqlException> sqlErrors = new java.util.LinkedHashMap<>() {
    @java.io.Serial private static final long serialVersionUID = 1L;

    @Override
    protected boolean removeEldestEntry(java.util.Map.Entry<Long, MleSqlException> eldest) {
      return size() > 64;
    }
  };

  /**
   * The Error of a routine that ended with an SQL error. With the id of an ERROR the host sent in
   * this top-level request, that ERROR's number, SQLSTATE and message are the Error's; otherwise
   * the error is the routine's own, raised like SIGNAL, and has no id.
   */
  private ErrorInfo sqlInfo(String language, String message, List<Frame> frames, int code, String state, long id) {
    MleSqlException sent = id == 0 ? null : sqlErrors.get(id);
    if (sent != null)
      return new ErrorInfo(language, sent.getMessage(), frames, sent.getErrorCode(), sent.getSqlState(), id);
    return new ErrorInfo(language, message, frames, code, state, 0);
  }

  /**
   * A guest SqlError (the SQL API's error class of JS and Python: marked with $mle$sql, with code
   * and sqlState, and $mle$id when it came from an SQL statement): an inner statement's error, or
   * one the routine throws (raised like SIGNAL).
   */
  private ErrorInfo sqlError(String language, org.graalvm.polyglot.Value g, List<Frame> frames) {
    try {
      if (g == null || !g.hasMembers()) return null;
      org.graalvm.polyglot.Value mark = g.getMember("$mle$sql");
      if (mark == null || !mark.isBoolean() || !mark.asBoolean()) return null;
      org.graalvm.polyglot.Value code = g.getMember("code");
      org.graalvm.polyglot.Value state = g.getMember("sqlState");
      org.graalvm.polyglot.Value message = g.getMember("message");
      org.graalvm.polyglot.Value sent = g.getMember("$mle$id");
      String text = message == null || message.isNull() ? "" : message.isString() ? message.asString()
          : message.toString();
      String st = state != null && state.isString() ? state.asString() : "";
      long id = 0;
      if (sent != null && sent.isString()) {
        try {
          id = Long.parseUnsignedLong(sent.asString());
        } catch (NumberFormatException e) {
          id = 0;
        }
      }
      if (id != 0 && sqlErrors.containsKey(id)) return sqlInfo(language, text, frames, 0, "", id);
      // A state the host would refuse (and end the worker for): an error of the routine then.
      if (!st.isEmpty() && !Text.sqlState(st))
        return new ErrorInfo(language, text + " (SqlError with the invalid SQLSTATE '" + st + "')", frames, 0, "", 0);
      return new ErrorInfo(language, text, frames, code != null && code.fitsInInt() ? code.asInt() : 0, st, 0);
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
        int line = Math.max(0, e.getLineNumber());
        frames.add(new Frame(function, file, line, 0, 0, o == null ? "" : o.line(line)));
      }
      if (frames.size() == Wire.MAX_STACK_FRAMES) break;
    }
    return frames;
  }

  /** MySQL's error for a packet larger than max_allowed_packet. */
  private static final int ER_NET_PACKET_TOO_LARGE = 1153;
  /** MySQL's errors for a statement handle that is not known, and for a killed statement. */
  private static final int ER_UNKNOWN_STMT_HANDLER = 1243;
  private static final int ER_QUERY_INTERRUPTED = 1317;

  private static String tooLarge(String what) {
    return what + " is larger than MLE allows (" + (Wire.MAX_WORKER_MESSAGE >> 20) + " MiB)";
  }

  private static ErrorInfo guest(String language, String message) {
    return new ErrorInfo(language, message, List.of(), 0, "", 0);
  }

  private static ErrorInfo internal(String message) {
    return new ErrorInfo("", message, List.of(), 0, "", 0);
  }

  // ------------------------------------------------------------ RESULT

  /**
   * An Error as §7.4 and §10.6 allow it, whatever guest code made of it: the language one of
   * READY's or "" (a marker only where it belongs), the texts cut to their limits, at most 1000
   * frames, an errno of at most 65535, an SQLSTATE that is one or empty.
   */
  private ErrorInfo fit(Request q, int status, ErrorInfo e) {
    String language = e.language() == null ? "" : e.language();
    boolean known = switch (language) {
      case Wire.LANGUAGE_TYPE -> status == Wire.GUEST_ERROR && q.op == Wire.CALL;
      case Wire.LANGUAGE_TIME_ZONE, Wire.LANGUAGE_OPTION -> status == Wire.GUEST_ERROR && q.op == Wire.OPTIONS;
      default -> languages.contains(language);
    };
    if (!known) language = "";
    List<Frame> frames = new ArrayList<>(Math.min(e.frames().size(), Wire.MAX_STACK_FRAMES));
    for (Frame f : e.frames()) {
      if (frames.size() == Wire.MAX_STACK_FRAMES) break;
      frames.add(new Frame(Text.cut(f.function() == null ? "" : f.function(), Wire.MAX_FRAME_TEXT),
          Text.cut(f.file() == null ? "" : f.file(), Wire.MAX_FRAME_TEXT), Math.max(0, f.line()),
          Math.max(0, f.column()), Math.max(0, f.endColumn()),
          Text.cut(f.text() == null ? "" : f.text(), Wire.MAX_FRAME_TEXT)));
    }
    int errno = e.sqlErrno() >= 0 && e.sqlErrno() <= Wire.MAX_SQL_ERRNO ? e.sqlErrno() : 0;
    String state = e.sqlState() != null && Text.sqlState(e.sqlState()) ? e.sqlState() : "";
    return new ErrorInfo(language, Text.cut(e.message() == null ? "" : e.message(), Wire.MAX_ERROR_MESSAGE), frames,
        errno, state, e.sqlErrorId());
  }

  private void result(Request q, int status, ErrorInfo error) {
    result(q, status, null, null, error, null, null, null, List.of());
  }

  /**
   * Posts the RESULT of q (§9.4): with status OK of a CALL, the return value (retType: a function)
   * and the OUT values outs[i] for every i in outIndexes; findings: of a PREPARE with check.
   *
   * <p>The RESULT of a PREPARE or CALL carries all console output no RESULT carried yet. A value
   * that does not convert, or a RESULT that would be larger than a worker's message may be, turns
   * into an error of language MLE-Type; before that, console output is cut to the end of each
   * stream. The RESULT of DROP, RESET and OPTIONS is one chunk: its Error loses its frames to fit.
   */
  private void result(Request q, int status, Object ret, Values.ParamType retType, ErrorInfo error, int[] outIndexes,
      Object[] outs, Values.ParamType[] types, List<String> findings) {
    q.activation = false;
    if (error != null) error = fit(q, status, error);
    if (q.op == Wire.DROP || q.op == Wire.RESET || q.op == Wire.OPTIONS) {
      try {
        w.reset(Wire.MAX_SMALL_MESSAGE);
        writeResult(null, null, error, null, null, null, NO_BYTES, 0, NO_BYTES, 0, false, List.of());
      } catch (MessageWriter.TooLarge e) {
        w.reset(Wire.MAX_SMALL_MESSAGE);
        writeResult(null, null, new ErrorInfo(error.language(), error.message(), List.of(), error.sqlErrno(),
            error.sqlState(), error.sqlErrorId()), null, null, null, NO_BYTES, 0, NO_BYTES, 0, false, List.of());
      }
    } else {
      boolean carries = q.op == Wire.PREPARE || q.op == Wire.CALL;
      byte[] out = carries ? console.takeOut() : NO_BYTES;
      byte[] err = carries ? console.takeErr() : NO_BYTES;
      boolean cleared = carries && console.takeCleared();
      if (!findings.isEmpty()) {
        List<String> cut = new ArrayList<>(Math.min(findings.size(), Wire.MAX_FINDINGS));
        for (String f : findings) {
          if (cut.size() == Wire.MAX_FINDINGS) break;
          cut.add(Text.cut(f, Wire.MAX_FINDING));
        }
        findings = cut;
      }
      int outFrom = 0, errFrom = 0;
      while (true) {
        ErrorInfo instead;
        try {
          w.reset();
          writeResult(ret, retType, error, outIndexes, outs, types, out, outFrom, err, errFrom, cleared, findings);
          break;
        } catch (Values.ConversionException e) {
          instead = guest(Wire.LANGUAGE_TYPE, e.getMessage());
        } catch (MessageWriter.TooLarge e) {
          // The console output is cut first; the RESULT without any of it is measured for that.
          try {
            w.reset();
            writeResult(ret, retType, error, outIndexes, outs, types, NO_BYTES, 0, NO_BYTES, 0, cleared, findings);
            long room = Wire.MAX_WORKER_MESSAGE - w.size();
            long outKeep = Math.min(out.length, Math.max(room / 2, room - err.length));
            long errKeep = Math.min(err.length, room - outKeep);
            outFrom = (int) (out.length - outKeep);
            errFrom = (int) (err.length - errKeep);
            continue;
          } catch (Values.ConversionException c) {
            instead = guest(Wire.LANGUAGE_TYPE, c.getMessage());
          } catch (MessageWriter.TooLarge t) {
            instead = guest(Wire.LANGUAGE_TYPE, tooLarge("the routine's result"));
          }
        }
        // Only an OK RESULT has values, and an Error fits: this is reached once.
        status = Wire.GUEST_ERROR;
        error = fit(q, status, instead);
        ret = null;
        retType = null;
        outIndexes = null;
        findings = List.of();
      }
    }
    // A PREPARE that does not answer OK leaves the id not prepared (§8.3).
    if (q.op == Wire.PREPARE && status != Wire.OK) unprepare(q.routine);
    q.answered = true;
    // tainted is set before the RESULT that the host reads it after (§11.5).
    mb.stats(q.depth == 0 ? Wire.STATE_IDLE : Wire.STATE_RUNNING, tainted);
    mb.send(Wire.RESULT, status, q.depth, q.seq, w);
  }

  private void writeResult(Object ret, Values.ParamType retType, ErrorInfo error, int[] outIndexes, Object[] outs,
      Values.ParamType[] types, byte[] out, int outFrom, byte[] err, int errFrom, boolean cleared,
      List<String> findings) {
    if (retType != null && error == null) {
      w.bool(true);
      Values.writeAs(w, ret, retType);
    } else {
      w.bool(false);
    }
    if (error == null && outIndexes != null) {
      // Every OUT and INOUT parameter, in ascending index.
      w.u32(outIndexes.length);
      for (int i : outIndexes) {
        w.u32(i);
        Values.writeAs(w, outs[i], types[i]);
      }
    } else {
      w.u32(0);
    }
    w.bytes(out, outFrom, out.length - outFrom);
    w.bytes(err, errFrom, err.length - errFrom);
    w.bool(cleared);
    w.u32(findings.size());
    for (String f : findings) w.str(f);
    if (error == null) {
      w.bool(false);
      return;
    }
    w.bool(true);
    w.str(error.language());
    w.str(error.message());
    w.u32(error.frames().size());
    for (Frame f : error.frames()) {
      w.str(f.function());
      w.str(f.file());
      w.u32(f.line());
      w.u32(f.column());
      w.u32(f.endColumn());
      w.str(f.text());
    }
    w.u32(error.sqlErrno());
    w.str(error.sqlState());
    w.u64(error.sqlErrorId());
  }

  // ------------------------------------------------------------ SQL

  /** A new statement id of the request whose guest code runs now (§10.1). */
  long newStatementId() {
    return serving("SQL can be run").newStatementId();
  }

  /** How sqlRequest gives row values: Java values (MleSql), or raw for com.percona.mle.sql. */
  static final int JAVA_VALUES = 0;
  /** Raw: DATE/TIME/DATETIME as Values.Temporal, TIMESTAMP as Values.Timestamp, VECTOR as float[]. */
  static final int RAW = 1;
  /** Raw, VECTOR as a little-endian heap ByteBuffer (JavaScript shares its memory: one copy). */
  static final int RAW_BUFFERS = 2;

  /** One SQL message to the host and its reply; throws MleSqlException for an SQL error. */
  MleSql.Reply sqlRequest(int action, long id, String text, Object[] params, int flags, int maxRows) {
    return sqlRequest(action, id, text, params, flags, maxRows, JAVA_VALUES, null, 0);
  }

  /**
   * What guest code may ask for in one SQL message (§10.2): a legal action, flags only at EXECUTE
   * and PREPARE, and nothing in a field the action does not use.
   */
  private static void checkSql(int action, String text, Object[] params, int flags, int maxRows, int rowWidth) {
    boolean hasText = text != null && !text.isEmpty();
    boolean hasParams = params != null && params.length > 0;
    boolean ok = switch (action) {
      case Wire.SQL_EXECUTE -> rowWidth == 0;
      case Wire.SQL_PREPARE -> !hasParams && maxRows == 0 && rowWidth == 0;
      case Wire.SQL_EXECUTE_PREPARED -> !hasText && flags == 0 && rowWidth == 0;
      case Wire.SQL_FETCH, Wire.SQL_NEXT_RESULT -> !hasText && !hasParams && flags == 0 && rowWidth == 0;
      case Wire.SQL_CLOSE -> !hasText && !hasParams && flags == 0 && maxRows == 0 && rowWidth == 0;
      case Wire.SQL_EXECUTE_MANY -> !hasText && flags == 0 && maxRows == 0;
      default -> false;
    };
    if (!ok || (flags & ~Wire.SQL_FLAG_TO_CLIENT) != 0 || maxRows < 0 || rowWidth < 0)
      throw new IllegalArgumentException("not an SQL request: action " + action + " with arguments it does not take");
    // Code is not repaired: another text would be another statement (§7.1).
    if (hasText && !Text.wellFormed(text))
      throw new IllegalArgumentException("the statement is not well-formed Unicode text (an unpaired surrogate)");
    // More values than one list may have (§10.6).
    if (params != null && params.length > Wire.MAX_VALUES)
      throw new MleSqlException(1210, "HY000", "Incorrect arguments: " + params.length
          + " values in one statement (at most " + Wire.MAX_VALUES + ")");
  }

  /**
   * form: JAVA_VALUES, RAW or RAW_BUFFERS. sink: the rows go to it while the reply is decoded,
   * not into Reply.rows (exp note 44 step 3): raw values with VECTORs as MemorySegment views of the
   * mailbox, valid only during the sink's call. rowWidth: EXECUTE_MANY (exp note 51): params are
   * rows of this many values; 0 otherwise.
   *
   * <p>Only the guest code of the request being served gets here with a message posted; the
   * statement's state is tracked by the request's Statements (§10.1). A statement of another
   * request fails without a message. After an interrupt was delivered, everything but CLOSE
   * fails without a message (§11.1).
   */
  MleSql.Reply sqlRequest(int action, long id, String text, Object[] params, int flags, int maxRows,
      int form, RowSink sink, int rowWidth) {
    Request q = serving("SQL can be run");
    checkSql(action, text, params, flags, maxRows, rowWidth);
    if (!q.owns(id))
      throw new MleSqlException(ER_UNKNOWN_STMT_HANDLER, "HY000",
          "the statement or result belongs to another routine call (or its id was not made by this one)");
    if (action != Wire.SQL_CLOSE && control.refuse())
      throw new MleSqlException(ER_QUERY_INTERRUPTED, "70100", "Query execution was interrupted");
    drainDeferred(q, id);
    Statements statements = q.statements();
    if (action == Wire.SQL_EXECUTE_PREPARED) params = statements.bind(id, params);
    try {
      w.reset();
      w.u8(action);
      w.u64(id);
      w.str(text == null ? "" : text);
      w.u32(params == null ? 0 : params.length);
      if (params != null) for (Object p : params) Values.writeParam(w, p);
      w.u8(flags);
      w.u32(maxRows);
      w.u32(rowWidth);
    } catch (MessageWriter.TooLarge e) {
      // MySQL's error for a packet over max_allowed_packet, but not its SQLSTATE 08S01 (a lost
      // connection to pools and drivers): the session is fine.
      throw new MleSqlException(ER_NET_PACKET_TOO_LARGE, "HY000", tooLarge("the SQL statement with its parameters"));
    }
    Object[] lastRow = action == Wire.SQL_EXECUTE_MANY && rowWidth > 0 && params != null && params.length >= rowWidth
        ? Arrays.copyOfRange(params, params.length - rowWidth, params.length) : null;
    Statements.Exchange x = statements.begin(action, id, flags);
    mb.send(Wire.SQL, Wire.OK, q.depth, q.seq, w);
    Mailbox.Message m = sqlResult(q);
    if (!m.discarded()) {
      try {
        return sqlReply(x, action, id, maxRows, lastRow, m, form, sink);
      } catch (OutOfMemoryError e) {
        // no memory for the rows: as for a reply that was discarded
      }
    }
    // A reply the worker could not hold (§5.3): the id's state is not known, so it is closed.
    Statements.Exchange close = statements.begin(Wire.SQL_CLOSE, id, 0);
    w.reset();
    w.u8(Wire.SQL_CLOSE);
    w.u64(id);
    w.str("");
    w.u32(0);
    w.u8(0);
    w.u32(0);
    w.u32(0);
    mb.send(Wire.SQL, Wire.OK, q.depth, q.seq, w);
    sqlReply(close, Wire.SQL_CLOSE, id, 0, null, sqlResult(q), RAW, null);
    throw new MleSqlException(ER_NET_PACKET_TOO_LARGE, "HY000",
        "the statement's result is larger than this worker can hold; the statement was closed");
  }

  /** Waits for the SQL_RESULT of q's SQL message, serving the requests nested in the statement (§10.6). */
  private Mailbox.Message sqlResult(Request q) {
    try {
      while (true) {
        Mailbox.Message m = mb.receive(q);
        if (m.op() == Wire.SQL_RESULT) return m;
        handle(m);
      }
    } catch (Throwable t) {
      // Without the reply the worker does not have the turn: it cannot go on.
      throw fatal("waiting for SQL_RESULT", t);
    }
  }

  // A statement run from text whose rows are not all read yet (exp note 44 step 3): mysqld
  // buffers the whole result, so before another statement of the request runs its rows are
  // drained into the worker. One at a time, per request: the host closes what a request leaves.

  /** Statement id's rows are drained by drain before the next message for another statement. */
  void deferDrain(long id, Runnable drain) {
    Request q = current;
    if (Thread.currentThread() != thread || q == null || !q.activation || !q.owns(id)) return;
    drainDeferred(q, id);
    q.deferred = drain;
    q.deferredId = id;
  }

  /** Statement id needs no draining any more (read to the end, or closed). */
  void drained(long id) {
    Request q = current;
    if (Thread.currentThread() != thread || q == null) return;
    if (q.deferred != null && q.deferredId == id) q.deferred = null;
  }

  private void drainDeferred(Request q, long forId) {
    if (q.deferred == null || q.deferredId == forId) return;
    Runnable d = q.deferred;
    q.deferred = null;
    d.run();
  }

  /** The mailbox arena's size: a batch is sent in messages that fit it (exp note 51). */
  int arenaSize() {
    return mb.arenaSize;
  }

  /** The request being served: no two share it, nested and sibling ones included. */
  long callScope() {
    Request q = current;
    return q == null ? 0 : q.scope;
  }

  /**
   * Whether the running top-level request was interrupted (polled by long waits in the worker:
   * mysql.sleep, the vector helper). Guest code is told, so the request ends INTERRUPTED.
   */
  boolean interrupted() {
    return control.refuse();
  }

  /** A RowSink is being given the rows of a reply that is read in place. */
  private boolean decodingRows;

  /** A result set's columns as the decoder keeps them for the set's later batches. */
  private record Columns(List<MleSql.Column> columns, int[] tags) {}

  private static void check(String violation) {
    if (violation != null) throw ProtocolFailure.exit("SQL_RESULT: " + violation);
  }

  /**
   * Decodes the SQL_RESULT m of an exchange and moves the statement to its next state. The whole
   * reply is checked and the state moved before anything is thrown that guest code can see: an
   * MleSqlException for ERROR, a failure of the sink, a value Java cannot show.
   */
  private MleSql.Reply sqlReply(Statements.Exchange x, int action, long id, int maxRows, Object[] lastRow,
      Mailbox.Message m, int form, RowSink sink) {
    MessageReader r = new MessageReader(m.body());
    int kind = r.u8();
    if (kind > Wire.SQL_END || !x.accepts(kind))
      throw ProtocolFailure.exit("SQL_RESULT of kind " + kind + " to action " + action
          + (x.allowed() ? "" : ", which the statement's state does not allow"));
    switch (kind) {
      case Wire.SQL_ERROR -> {
        long errno = r.u32();
        String sqlState = r.str();
        String message = r.str();
        long row = r.u32();
        long errorId = r.u64();
        r.finish();
        if (errno < 1 || errno > Wire.MAX_SQL_ERRNO || !Text.sqlState(sqlState) || errorId == 0
            || (action != Wire.SQL_EXECUTE_MANY && row != Wire.NO_ROW))
          throw ProtocolFailure.exit("SQL_RESULT ERROR " + errno + " '" + sqlState + "' with fields outside §10.4");
        x.error();
        MleSqlException e = new MleSqlException((int) errno, sqlState, message,
            action == Wire.SQL_EXECUTE_MANY ? row : -1, errorId);
        sqlErrors.put(errorId, e);
        throw e;
      }
      case Wire.SQL_OK -> {
        long affected = r.u64();
        long insertId = r.u64();
        List<MleSql.Warning> warnings = warnings(r);
        boolean moreResults = r.bool();
        long paramCount = r.u32();
        long execUs = r.u64();
        r.finish();
        check(x.ok(moreResults, paramCount, affected == 0 && insertId == 0 && warnings.isEmpty() && execUs == 0,
            lastRow));
        return new MleSql.Reply(id, false, List.of(), List.of(), false, moreResults, affected, insertId, warnings,
            (int) paramCount, execUs, 0, false);
      }
      case Wire.SQL_ROWS -> {
        int ncols = r.count(50, Wire.MAX_COLUMNS);
        if (x.firstBatch() != ncols > 0)
          check(ncols > 0 ? "columns in a later batch of a result set" : "no columns in the first batch of a result set");
        Columns set;
        if (ncols > 0) {
          List<MleSql.Column> columns = new ArrayList<>(ncols);
          int[] tags = new int[ncols];
          for (int i = 0; i < ncols; i++) {
            String name = r.str();
            String table = r.str();
            Values.ParamType t = Values.readColumnType(r);
            tags[i] = t.tag();
            columns.add(new MleSql.Column(name, table, t.sqlName(), t.unsigned(), t.length(), (int) t.scale(),
                r.str(), r.str(), r.str(), r.str(), r.str(), (int) r.u32()));
          }
          set = new Columns(List.copyOf(columns), tags);
        } else {
          set = (Columns) x.columns();
          if (set == null) check("a later batch of a result set whose columns were not sent");
        }
        int[] tags = set.tags();
        ncols = tags.length;
        long total = r.u64();
        long nrows = r.u32();
        if (nrows > r.remaining() / ncols) check(nrows + " rows of " + ncols + " values in " + r.remaining() + " bytes");
        if (maxRows != 0 && nrows > maxRows) check(nrows + " rows where max_rows is " + maxRows);
        if (nrows > 1 && m.chunked()) check("a batch of " + nrows + " rows in more than one chunk");
        List<Object[]> rows = sink != null ? List.of() : new ArrayList<>((int) nrows);
        Object[] row = sink != null ? new Object[ncols] : null;
        Throwable sinkFailure = null;
        for (long i = 0; i < nrows; i++) {
          if (sink == null) row = new Object[ncols];
          for (int c = 0; c < ncols; c++) {
            Object v = Values.read(r, tags[c]);
            row[c] = v instanceof Values.VectorRef ref ? vector(ref, form, sink != null) : v;
          }
          if (sink == null) {
            rows.add(row);
          } else if (sinkFailure == null) {
            decodingRows = true;
            try {
              sink.row(row);
            } catch (OutOfMemoryError e) {
              throw e;
            } catch (Throwable t) {
              sinkFailure = t;  // thrown when the reply is read to its end
            } finally {
              decodingRows = false;
            }
          }
        }
        boolean more = r.bool();
        boolean moreResults = r.bool();
        List<MleSql.Warning> warnings = warnings(r);
        long execUs = r.u64();
        r.finish();
        check(x.rows(x.firstBatch(), set, total, nrows, more, moreResults));
        if (sinkFailure instanceof RuntimeException e) throw e;
        if (sinkFailure instanceof Error e) throw e;
        if (sinkFailure != null) throw new IllegalStateException(sinkFailure);
        if (form == JAVA_VALUES)
          for (Object[] values : rows)
            for (int c = 0; c < ncols; c++) values[c] = Values.toJava(values[c]);
        return new MleSql.Reply(id, true, set.columns(), rows, more, moreResults, 0, 0, warnings, 0, execUs,
            total < 0 ? -1 : total, false);
      }
      default -> {
        r.finish();
        x.end();
        return new MleSql.Reply(id, false, List.of(), List.of(), false, false, 0, 0, List.of(), 0, 0, 0, true);
      }
    }
  }

  /** A VECTOR of a result as sqlRequest's form asks (view: the mailbox's bytes, for a sink). */
  private static Object vector(Values.VectorRef ref, int form, boolean view) {
    if (view) return ref.data();
    if (form != RAW_BUFFERS) return ref.toArray();
    ByteBuffer b = ByteBuffer.allocate((int) ref.data().byteSize()).order(ByteOrder.LITTLE_ENDIAN);
    MemorySegment.ofBuffer(b).copyFrom(ref.data());
    return b;
  }

  private static List<MleSql.Warning> warnings(MessageReader r) {
    int n = r.count(12, Wire.MAX_WARNINGS);
    if (n == 0) return List.of();
    List<MleSql.Warning> list = new ArrayList<>(n);
    for (int i = 0; i < n; i++) {
      long level = r.u32();
      if (level > 2) throw ProtocolFailure.exit("a Warning of level " + level);
      list.add(new MleSql.Warning((int) level, (int) r.u32(), r.str()));
    }
    return list;
  }
}
