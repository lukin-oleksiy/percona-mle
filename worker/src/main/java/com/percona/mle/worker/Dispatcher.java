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
 * <p>The host is trusted (§1). Of its messages the worker checks what keeps a mistaken host from
 * making it unsafe: lengths and counts against the bytes of the message; every tag, kind, status,
 * mode, action and operation; the numbers it finds its own state by (a routine id, an argument
 * count, a row's values against the set's columns, failed_row, UNCHANGED against a body it
 * holds); and that its state can take the message (operation, depth, call_seq, the reply kind for
 * the action). Such a message ends the process (ProtocolFailure): it is never an exception that
 * guest code could see. The domains of values, the form of text, reserved and unused fields and
 * what the host guarantees about order and uniqueness are not checked.
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
  /** Once set, every RESULT carries the TAINTED flag: the host retires the worker (§11.5). */
  private boolean tainted;
  /** RESET asked for a GC (memory back before the worker waits); run when idle, not on the way. */
  private boolean gcWhenIdle;
  /**
   * RESET ended the options in force and the languages still have the session's: they get the
   * defaults when a request that runs guest code comes first, before it. The next session's
   * OPTIONS usually names the same zone, which then costs the languages nothing.
   */
  private boolean defaultsDue;
  /** How long a reset worker waits for a request before it collects (exp note 38). */
  private static final long IDLE_GC_NS = 200_000_000L;
  /**
   * mle.idle_trim_ms: a worker with Python set up gives memory back when it is idle in a session
   * (exp note 68 §8): when no request comes for this long after a top-level request, a full GC
   * and malloc_trim, once per idle period. GraalPy's heap and native memory gave back 60-80 MB;
   * Java and JavaScript gave back nothing, so a worker without Python is not trimmed. The wait
   * sleeps on the mailbox's futex: a request wakes it at once (a 1 ms poll cost every call 1 ms).
   * 0: never.
   */
  private static final long IDLE_TRIM_NS = Long.getLong("mle.idleTrimMs", 0) * 1_000_000L;
  /** A top-level request ended since the last trim (or RESET's GC). */
  private boolean trimWhenIdle;
  private final ThreadWatch threads = new ThreadWatch();
  private static final byte[] NO_BYTES = new byte[0];
  private static final Object[] NO_VALUES = new Object[0];

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
    int op;
    final int depth;
    long seq;
    long scope;
    /** Reads the request's body, and later the replies to its SQL and LIBRARY messages. */
    final MessageReader reader = new MessageReader();
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

    Request(int depth) {
      this.depth = depth;
    }

    /** The object serves the next request at its depth: one per depth, none made per request. */
    Request begin(int op, long seq) {
      this.op = op;
      this.seq = seq;
      scope = ++serials;
      idScope = scope;
      ids = 0;
      moreScopes = null;
      routine = 0;
      epoch = 0;
      schema = null;
      activation = false;
      answered = false;
      if (statements != null) statements.clear();
      deferred = null;
      return this;
    }

    /** A request starts with no statement ids (§10.1); the object is kept for the next one. */
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

    final Mailbox.Admission libraryResult = this::checkLibraryResult;

    /** While this request waits for a LIBRARY_RESULT: nothing else. */
    void checkLibraryResult(int o, int d, long s) {
      if (o != Wire.LIBRARY_RESULT || d != depth || s != seq)
        throw ProtocolFailure.exit("op " + o + " at depth " + d + " while request " + Long.toUnsignedString(seq)
            + " waits for LIBRARY_RESULT at depth " + depth);
    }
  }

  /** The request objects, by depth. */
  private final Request[] requests = new Request[Wire.MAX_DEPTH + 1];
  private final Mailbox.Admission topLevel = this::checkTopLevel;

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

  /** The worker_class of READY: this kind of worker, whatever the host calls its class (§9.1). */
  private static final String WORKER_CLASS = "jvm";

  /**
   * A failed READY for a region of a major version this worker does not implement (§2 step 3,
   * §14): before the runtime is set up, so that nothing of it can fail first (MVP review CONF-7).
   * With INTERNAL the body may end after info (§9.1).
   */
  static void readyOtherMajor(Mailbox mb) {
    MessageWriter w = new MessageWriter(mb.arena);
    w.reset(Wire.MAX_SMALL_MESSAGE);
    w.u32(Wire.VERSION);
    w.str("worker speaks " + Wire.VERSION_MAJOR + ".0, host " + (mb.version >>> 16) + "." + (mb.version & 0xFFFF));
    mb.send(Wire.READY, Wire.INTERNAL, 0, 0, 0, w);
  }

  /** Posts READY (§9.1): one chunk; with a status other than OK, info is why the worker cannot serve. */
  void ready(int status, String info) {
    w.reset(Wire.MAX_SMALL_MESSAGE);
    w.u32(Wire.VERSION);
    w.str(Text.cut(info, Wire.MAX_ERROR_MESSAGE));
    w.u64(0);  // features: version 1.0 defines none
    w.str(WORKER_CLASS);
    w.u32(languages.size());
    for (String l : languages) w.str(l);
    w.str(Text.cut("Java " + Runtime.version() + " (" + System.getProperty("java.vm.name") + ")", Wire.MAX_FRAME_TEXT));
    mb.stats(Wire.STATE_IDLE);
    mb.send(Wire.READY, status, 0, 0, 0, w);
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
          Native.mallocTrim();
        }
        // Idle after a RESET (§3.1): the host's memory figures, after the GC if there was one.
        mb.stats(Wire.STATE_IDLE);
      } else if (trimWhenIdle && IDLE_TRIM_NS > 0 && polyglot.pythonStarted()) {
        trimWhenIdle = false;
        if (!mb.awaitMessage(IDLE_TRIM_NS)) {
          MemoryWatch.forcedGc();
          System.gc();
          Native.mallocTrim();
          mb.stats(Wire.STATE_IDLE);
        }
      }
      serveNext();
    }
  }

  /** Receives the next top-level request and serves it (also the training run's, Train). */
  void serveNext() {
    Mailbox.Message m = mb.receive(topLevel);
    lastSeq = m.callSeq();
    handle(m);
    if (!gcWhenIdle) trimWhenIdle = true;
  }

  /**
   * While the worker is idle (§8.1): a top-level request with the next call_seq. Whether options
   * are in force is not checked (§1): without them the worker serves with +00:00 and the defaults.
   */
  private void checkTopLevel(int op, int depth, long seq) {
    boolean request = op == Wire.PREPARE || op == Wire.CALL || op == Wire.PARSE_LIBRARY;
    if (!(request || op == Wire.DROP || op == Wire.RESET || op == Wire.OPTIONS) || depth != 0 || seq != lastSeq + 1)
      throw ProtocolFailure.exit("op " + op + " at depth " + depth + " with call_seq " + Long.toUnsignedString(seq)
          + " while idle after request " + Long.toUnsignedString(lastSeq));
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
    // The admission checked the depth: 0, or one more than the request that waits.
    Request q = requests[m.depth()];
    if (q == null) q = requests[m.depth()] = new Request(m.depth());
    q.begin(m.op(), m.callSeq());
    current = q;
    boolean top = q.depth == 0;
    if (top) {
      if (defaultsDue && q.op != Wire.OPTIONS && q.op != Wire.DROP && q.op != Wire.RESET) {
        defaultsDue = false;
        polyglot.applyOptions();
      }
      FrameLimit.reset();
      LibraryFileSystem.requestStart();
      sqlErrors.clear();
      topRequest++;
    }
    try {
      // OPTIONS, DROP and RESET are at most 64 KiB: a worker that cannot hold even these is in
      // no state to serve (§5.3).
      if (m.discarded() && (q.op == Wire.DROP || q.op == Wire.RESET || q.op == Wire.OPTIONS)) {
        if (top) control.begin(q.seq, false, false);
        tainted = true;
        result(q, Wire.INTERNAL, internal("the request is larger than this worker can hold"));
        return;
      }
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
    long id = q.reader.reset(m.body, m.length).u64();
    if (q.depth == 0) control.begin(q.seq, false, false);
    unprepare(id);
    LibraryFileSystem.discard(id);
    result(q, Wire.OK, null);
  }

  private void reset(Request q, Mailbox.Message m) {
    control.begin(q.seq, false, false);
    java.reset();
    polyglot.reset();
    schemas.clear();
    LibraryFileSystem.reset();
    // What guest code printed and no RESULT carried is dropped; the options end (in the
    // languages when guest code runs next: defaultsDue).
    console.discard();
    options.reset();
    defaultsDue = true;
    // A worker with a thread a routine left is not kept for reuse (the host retires it).
    if (!tainted && threads.strangers()) tainted = true;
    // The host took the memory watcher's count with the session's statistics.
    MemoryWatch.clear();
    // A reset worker is usually parked for reuse: give its memory back (12 §17), once idle.
    gcWhenIdle = true;
    result(q, Wire.OK, null);
  }

  /**
   * OPTIONS (§9.8): a setting that is absent has its default. Names this worker does not know are
   * ignored. A zone it does not know or a value it does not accept leaves the options as they were;
   * one answered OK replaces them (the host sends it only while no id is prepared).
   */
  private void options(Request q, Mailbox.Message m) {
    MessageReader r = q.reader.reset(m.body, m.length);
    String zoneText = r.str();
    int n = r.count(8);
    int integerType = Options.INT_UNSAFE_STRING;
    int decimalType = Options.DECIMAL_STRING;
    String refused = null;
    for (int i = 0; i < n; i++) {
      String name = r.str();
      String value = r.str();
      int v;
      if (name.equals(Options.INTEGER_TYPE)) {
        if ((v = Options.integerType(value)) >= 0) integerType = v;
        else if (refused == null) refused = name;
      } else if (name.equals(Options.DECIMAL_TYPE)) {
        if ((v = Options.decimalType(value)) >= 0) decimalType = v;
        else if (refused == null) refused = name;
      }
    }
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
    options.zone(zoneText, zone);
    defaultsDue = false;
    polyglot.applyOptions();
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
        MessageReader r = q.reader.reset(m.body, m.length);
        language = r.str();
        name = r.str();
        body = r.str();
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
    MemoryWatch.phase(Wire.STATE_COMPILING);
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
    mb.send(Wire.LIBRARY, Wire.OK, 0, q.depth, q.seq, w);
    Mailbox.Message m;
    try {
      m = mb.receive(q.libraryResult);
    } catch (Throwable t) {
      throw fatal("waiting for LIBRARY_RESULT", t);
    }
    // A body the worker has no memory for: as if the host had refused the import (§5.3).
    if (m.discarded())
      return new LibraryFileSystem.Reply(Wire.LIBRARY_REFUSED, "", "", 0, "the library is larger than this worker can hold");
    try {
      MessageReader r = q.reader.reset(m.body, m.length);
      int status = r.u8();
      String language = r.str();
      String body = r.str();
      long version = r.u64();
      String message = r.str();
      // The status is one of the three, and UNCHANGED is for a body the worker holds (§1).
      boolean ok = switch (status) {
        case Wire.LIBRARY_REFUSED, Wire.LIBRARY_BODY -> true;
        case Wire.LIBRARY_UNCHANGED -> knownVersion != 0 && version == knownVersion;
        default -> false;
      };
      if (!ok) throw ProtocolFailure.exit("a LIBRARY_RESULT with status " + status + " that the worker cannot take");
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
      int n = r.count(23);
      List<JavaRoutines.Param> params = new ArrayList<>(n);
      for (int i = 0; i < n; i++) {
        String param = r.str();
        int mode = r.u8();
        if (mode > Wire.MODE_INOUT) throw ProtocolFailure.exit("a Param with mode " + mode);
        params.add(new JavaRoutines.Param(param, mode, Values.readType(r)));
      }
      Values.ParamType ret = r.bool() ? Values.readType(r) : null;
      int ni = r.count(12);
      List<LibraryFileSystem.Import> imports = new ArrayList<>(ni);
      for (int i = 0; i < ni; i++) imports.add(new LibraryFileSystem.Import(r.str(), r.str(), r.str()));
      String schema = r.str();
      boolean check = r.bool();
      long epoch = r.u64();
      return new Prepare(language, name, entry, source, params, ret, imports, schema, check, epoch);
    }
  }

  private void prepare(Request q, Mailbox.Message m) {
    boolean top = q.depth == 0;
    // Of a message the worker could not hold, the first bytes are kept: the routine_id.
    MessageReader r = q.reader.reset(m.body, m.length);
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
    // <eval> of a session without a current schema has none (§9.2).
    String schema = p.schema().isEmpty() ? null : p.schema();
    q.schema = schema;
    schemas.put(id, schema);
    // Loading runs routine code too (Python module level, JS top level), so PREPARE can be
    // interrupted like a call. A nested request that arrives after the interrupt runs nothing.
    if (top) {
      control.begin(q.seq, true, !javaRoutine);
    } else if (control.refuse()) {
      result(q, Wire.INTERRUPTED, guest("", "interrupted"));
      return;
    }
    MemoryWatch.phase(Wire.STATE_COMPILING);
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
      MessageReader r = q.reader.reset(m.body, m.length);
      long id = r.u64();
      q.routine = id;
      q.epoch = r.u64();
      q.schema = schemas.get(id);
      int n = r.count(1);
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
        Object[] a = n == 0 ? NO_VALUES : new Object[n];
        for (int i = 0; i < n; i++) a[i] = Values.read(r);
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
    MemoryWatch.phase(Wire.STATE_RUNNING);
    // The call's arena owns the MemorySegment arguments of a Java routine (its VECTOR
    // parameters); closing it makes them unusable afterwards. Other calls make none.
    try (Arena callArena = routine != null && hasVector(routine.types()) ? Arena.ofConfined() : null) {
      Object ret = null;
      // Indexed by parameter: made only for a routine with OUT or INOUT parameters.
      int outCount = routine != null ? routine.outs().length : guestRoutine.outs().length;
      Object[] outs = outCount == 0 ? NO_VALUES : new Object[args.length];
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
      // From here on the routine's code gets an error for SQL and imports.
      q.activation = false;
      // The RESULT is composed in one pass with its values (§4.1). JavaScript and Python values
      // were converted to Java ones inside the call, where guest code may run. A Java routine's
      // JDK values are read as they are written; one of the routine's own classes is converted
      // here, while the request still counts for interrupts.
      if (failure == null && routine != null) {
        try {
          if (routine.ret() != null) ret = Values.settle(ret, routine.ret());
          for (int i : routine.outs()) outs[i] = Values.settle(outs[i], routine.types()[i]);
        } catch (Throwable t) {
          failure = t;
        }
      }
      boolean stopped = top ? control.end() : control.stopped();
      if (top) afterTopLevel();
      // The routine was stopped: also when its code went on and returned (§11.1).
      if (failure != null) failed(q, failure, stopped);
      else if (stopped) result(q, Wire.INTERRUPTED, guest("", "interrupted"));
      else if (routine != null) result(q, Wire.OK, ret, routine.ret(), null, routine.outs(), outs, routine.types(), List.of());
      else result(q, Wire.OK, ret, guestRoutine.ret(), null, guestRoutine.outs(), outs, guestRoutine.params(), List.of());
    }
  }

  private static boolean hasVector(Values.ParamType[] types) {
    for (Values.ParamType t : types)
      if (t.tag() == Wire.T_VECTOR) return true;
    return false;
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
    // Java's limits (Appendix B.5): the JVM's state may be damaged, so the worker is retired.
    if (t instanceof StackOverflowError) tainted = true;
    if (stopped && !(t instanceof OutOfMemoryError)) {
      result(q, Wire.INTERRUPTED, guest("java", "interrupted"));
      return;
    }
    if (t instanceof OutOfMemoryError || t instanceof StackOverflowError) {
      tainted = true;  // the host retires this worker after the top-level request
      result(q, Wire.EXHAUSTED, new ErrorInfo("java", t.toString(), userFrames(t), 0, "", 0));
    } else if (t instanceof MleSqlException e) {
      result(q, Wire.GUEST_ERROR, sqlInfo("java", e.getMessage(), userFrames(t), e.getErrorCode(), e.getSqlState(),
          heldId(e)));
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
    // Guest code ended the interpreter (Python's sys.exit, a hard exit): a failure it brought
    // about in the worker itself is an error of guest code, and the worker asks to be retired,
    // for the runtime may not run anything again (§10.6).
    if (e.isExit()) {
      tainted = true;
      result(q, Wire.GUEST_ERROR, new ErrorInfo(language, "the routine ended the interpreter (exit status "
          + e.getExitStatus() + ")", frames, 0, "", 0));
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
            heldId(sql)));
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
   * The number of the top-level request (one more for each): a guest SqlError's $mle$id is
   * "topRequest:error_id", so that one kept from an earlier request is not taken for an ERROR of
   * this one (§10.8).
   */
  static long topRequest;

  /** The $mle$id of a guest SqlError made from e in this top-level request; null without an id. */
  static String guestErrorId(MleSqlException e) {
    return e.getErrorId() == 0 ? null : topRequest + ":" + Long.toUnsignedString(e.getErrorId());
  }

  /**
   * The ERRORs of SQL_RESULTs of the top-level request by their error_id (§10.4), the most recent
   * ones: a routine that ends with one of them hands it back by its id (§10.8); one from an earlier
   * top-level request is not handed back (Java: not the same object; JS, Python: $mle$id of
   * another request).
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

  /** e's error_id if e is the ERROR held under it in this top-level request, else 0. */
  private long heldId(MleSqlException e) {
    long id = e.getErrorId();
    return id != 0 && sqlErrors.get(id) == e ? id : 0;
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
      String prefix = topRequest + ":";
      if (sent != null && sent.isString() && sent.asString().startsWith(prefix)) {
        try {
          id = Long.parseUnsignedLong(sent.asString().substring(prefix.length()));
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
      case Wire.LANGUAGE_TYPE -> status == Wire.GUEST_ERROR && (q.op == Wire.CALL || q.op == Wire.PREPARE);
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
        } catch (OutOfMemoryError e) {
          // A value too large for the heap (§9.4, Appendix B.5): EXHAUSTED, and the worker is
          // retired (MVP review JVMP-1).
          tainted = true;
          instead = guest(Wire.LANGUAGE_TYPE, "out of memory while the routine's result was written");
          if (status == Wire.OK) status = Wire.EXHAUSTED;
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
          } catch (RuntimeException r) {
            instead = guest(Wire.LANGUAGE_TYPE, "cannot write the routine's result: " + r);
          }
        } catch (RuntimeException e) {
          // What a value of guest code makes the writer throw (a closed arena's segment, an
          // MleJson without text): an error of guest code, not INTERNAL (§10.6; MVP review JVMP-1).
          instead = guest(Wire.LANGUAGE_TYPE, "cannot write the routine's result: " + e);
        }
        // Only an OK RESULT has values, and an Error fits: this is reached once.
        if (status != Wire.EXHAUSTED) status = Wire.GUEST_ERROR;
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
    // A worker that must not be reused says so in this RESULT and in every later one (§11.5).
    mb.send(Wire.RESULT, status, tainted ? Wire.FLAG_TAINTED : 0, q.depth, q.seq, w);
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
    int sent = params == null ? 0 : params.length;
    mb.send(Wire.SQL, Wire.OK, 0, q.depth, q.seq, w);
    Mailbox.Message m = sqlResult(q);
    if (!m.discarded()) {
      try {
        return sqlReply(q, x, action, id, rowWidth > 0 ? sent / rowWidth : 0, lastRow, m, form, sink);
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
    mb.send(Wire.SQL, Wire.OK, 0, q.depth, q.seq, w);
    sqlReply(q, close, Wire.SQL_CLOSE, id, 0, null, sqlResult(q), RAW, null);
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

  /**
   * Decodes the SQL_RESULT m of an exchange and moves the statement to its next state. Checked
   * are the reply's kind against the action and the statement's state, its counts against its
   * bytes (values are decoded by their own tags, §1 item 2), failed_row against the rows sent (rowsSent, of
   * an EXECUTE_MANY) and that a batch has columns: its own, its set's, or for a prepared id the
   * ones the worker keeps for it (§10.3). The state is moved before anything is thrown that
   * guest code can see: an MleSqlException for ERROR, a failure of the sink, a value Java cannot
   * show.
   */
  private MleSql.Reply sqlReply(Request q, Statements.Exchange x, int action, long id, long rowsSent,
      Object[] lastRow, Mailbox.Message m, int form, RowSink sink) {
    MessageReader r = q.reader.reset(m.body, m.length);
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
        if (action == Wire.SQL_EXECUTE_MANY && row > rowsSent)
          throw ProtocolFailure.exit("SQL_RESULT ERROR at row " + row + " of " + rowsSent);
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
        x.ok(moreResults, lastRow);
        return new MleSql.Reply(id, false, List.of(), List.of(), false, moreResults, affected, insertId, warnings,
            (int) paramCount, execUs, 0, false);
      }
      case Wire.SQL_ROWS -> {
        int ncols = r.count(50);
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
          // A later batch of the set; or a set of a prepared id with the columns sent last for it.
          set = (Columns) x.columns();
          if (set == null) throw ProtocolFailure.exit("SQL_RESULT: rows without columns");
        }
        int[] tags = set.tags();
        ncols = tags.length;
        long total = r.u64();
        long nrows = r.u32();
        // Every value takes at least its tag: the rows are checked against the bytes left.
        if (nrows > r.remaining() / ncols)
          throw ProtocolFailure.exit("SQL_RESULT: " + nrows + " rows of " + ncols + " values in " + r.remaining() + " bytes");
        List<Object[]> rows = sink != null ? List.of() : new ArrayList<>((int) nrows);
        Object[] row = sink != null ? new Object[ncols] : null;
        Throwable sinkFailure = null;
        for (long i = 0; i < nrows; i++) {
          if (sink == null) row = new Object[ncols];
          for (int c = 0; c < ncols; c++) {
            Object v = Values.read(r);
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
        x.rows(set, more, moreResults);
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
    int n = r.count(12);
    if (n == 0) return List.of();
    List<MleSql.Warning> list = new ArrayList<>(n);
    for (int i = 0; i < n; i++) list.add(new MleSql.Warning((int) r.u32(), (int) r.u32(), r.str()));
    return list;
  }
}
