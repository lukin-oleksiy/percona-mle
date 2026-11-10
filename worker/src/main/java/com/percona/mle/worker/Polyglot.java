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

import com.percona.mle.api.MleJson;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.time.Duration;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.ZoneOffset;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.EnvironmentAccess;
import org.graalvm.polyglot.HostAccess;
import org.graalvm.polyglot.PolyglotAccess;
import org.graalvm.polyglot.PolyglotException;
import org.graalvm.polyglot.Source;
import org.graalvm.polyglot.Value;
import org.graalvm.polyglot.io.FileSystem;
import org.graalvm.polyglot.io.IOAccess;
import org.graalvm.polyglot.proxy.ProxyExecutable;

/**
 * JS and Python routines: one polyglot Context per worker (created on first use), shared with
 * Java routines (Mle.context()), so cross-language calls stay inside the process.
 *
 * <p>A routine's source defines a function named by the entry. JS sources are evaluated in their
 * own function scope, Python sources in their own module namespace; globalThis and Python
 * builtins are shared by the session (Oracle-style scoping rules come in Phase 4 and 7).
 *
 * <p>Value conversions (Phase 3 defaults; the Oracle rules come with Phase 4, Python's with 7):
 * integers as numbers (JS: as strings beyond 2^53), DECIMAL as string (JS) / Decimal (Python),
 * dates as Date / datetime (UTC), TIME as string / timedelta, JSON parsed, BLOB as Uint8Array /
 * bytes, VECTOR as Float32Array / numpy float32 array (array.array without numpy).
 *
 * <p>System properties: mle.python.venv (a GraalPy venv, e.g. with numpy).
 */
final class Polyglot {
  static final List<String> LANGUAGES = List.of("js", "python");

  record Routine(String language, Value fn, Value[] converters, Values.ParamType ret) {}

  private final Console console;
  private final Map<Long, Routine> routines = new HashMap<>();
  private volatile Context context;
  private Helpers js;
  private Helpers python;
  private Value mleApi;

  /** Per language: argument converters and result extractors written in the language. */
  private record Helpers(Value bytes, Value vector, Value json, Value date, Value datetime, Value time,
      Value decimal, Value bigint, Value str, Value toJson, Value toBytes, Value toVector, Value toNumber,
      Value loader) {}

  Polyglot(Console console) {
    this.console = console;
  }

  static boolean isGuest(String language) {
    return LANGUAGES.contains(language);
  }

  Context contextIfCreated() {
    return context;
  }

  /**
   * After a Context.interrupt: the interrupt completes only when every entered thread has left the
   * context, and the main thread stays entered between calls; so leave, let the interrupt finish,
   * and enter again (only after a KILL, so the cost does not matter).
   */
  void settleInterrupt(Thread interrupter) {
    Context c = context;
    if (c == null || interrupter == null) return;
    c.leave();
    try {
      interrupter.join(10_000);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    } finally {
      c.enter();
    }
  }

  Context context() {
    if (context == null) {
      Context c = builder(console).build();
      // The worker's main thread stays entered: entering and leaving on every call costs about
      // 1 us (measured, W5). Everything that runs guest code runs on this thread.
      c.enter();
      // The static side of com.percona.mle.api.Mle: Mle.sql(...) in JS and Python.
      mleApi = c.asValue(com.percona.mle.api.Mle.class).getMember("static");
      c.getBindings("js").putMember("Mle", mleApi);
      c.getPolyglotBindings().putMember("Mle", mleApi);
      js = jsHelpers(c);
      python = pythonHelpers(c);
      context = c;
    }
    return context;
  }

  /**
   * The Context configuration; also used by Setup at build time. The language-home and
   * internal-resource file system wrappers are deprecated in GraalVM 25 (to replace later); the OS
   * lockdown, not this file system, is the boundary.
   */
  @SuppressWarnings("deprecation")
  static Context.Builder builder(Console console) {
    Context.Builder b = Context.newBuilder("js", "python")
        .allowHostAccess(HostAccess.ALL)
        // The process is the sandbox; only the worker's own classes are hidden.
        .allowHostClassLookup(name -> !name.startsWith("com.percona.mle.worker."))
        .allowPolyglotAccess(PolyglotAccess.ALL)
        .allowCreateThread(true)  // GraalPy's C API helper thread
        .allowCreateProcess(false)
        .allowNativeAccess(true)  // numpy and other native extensions
        .allowEnvironmentAccess(EnvironmentAccess.NONE)
        .allowIO(IOAccess.newBuilder()
            .fileSystem(FileSystem.allowInternalResourceAccess(FileSystem.allowLanguageHomeAccess(
                FileSystem.newReadOnlyFileSystem(FileSystem.newDefaultFileSystem()))))
            .allowHostSocketAccess(false)
            .build())
        .option("engine.WarnInterpreterOnly", "false")
        .option("python.PosixModuleBackend", "java")
        .option("python.DontWriteBytecodeFlag", "true")
        .option("python.NoUserSiteFlag", "true")
        .option("python.WarnExperimentalFeatures", "false")
        .environment("HOME", "/nonexistent");  // GraalPy looks up the home directory (U-1)
    String venv = System.getProperty("mle.python.venv", "");
    if (!venv.isEmpty())
      b.option("python.Executable", Path.of(venv).resolve("bin/python").toString())
          .option("python.ForceImportSite", "true");
    if (console != null) b.out(console.outStream()).err(console.errStream());
    return b;
  }

  private static Helpers jsHelpers(Context c) {
    Value h = c.eval(Source.create("js", """
        ({
          bytes: (b) => new Uint8Array(new ArrayBuffer(b)),
          vector: (b) => new Float32Array(new ArrayBuffer(b)),
          json: (s) => JSON.parse(s),
          date: (ms) => new Date(ms),
          datetime: (ms) => new Date(ms),
          time: (s) => s,
          decimal: (s) => s,
          bigint: (s) => s,
          str: (v) => String(v),
          toJson: (v) => JSON.stringify(v),
          toBytes: (v, alloc) => {
            const u = v instanceof Uint8Array ? v
                : ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength)
                : v instanceof ArrayBuffer ? new Uint8Array(v) : Uint8Array.from(v);
            const b = alloc(u.byteLength);
            new Uint8Array(new ArrayBuffer(b)).set(u);
            return b;
          },
          toVector: (v, alloc) => {
            const f = v instanceof Float32Array ? v : Float32Array.from(v);
            const b = alloc(f.byteLength);
            new Uint8Array(new ArrayBuffer(b)).set(new Uint8Array(f.buffer, f.byteOffset, f.byteLength));
            return b;
          },
          toNumber: (v) => Number(v),
          loader: null,
        })
        """));
    return new Helpers(h.getMember("bytes"), h.getMember("vector"), h.getMember("json"), h.getMember("date"),
        h.getMember("datetime"), h.getMember("time"), h.getMember("decimal"), h.getMember("bigint"),
        h.getMember("str"), h.getMember("toJson"), h.getMember("toBytes"), h.getMember("toVector"),
        h.getMember("toNumber"), null);
  }

  private static Helpers pythonHelpers(Context c) {
    Value ns = c.eval(Source.create("python", """
        import array as _array, datetime as _dt, decimal as _decimal, json as _json

        def _vector(b):
            data = bytes(b)
            try:
                import numpy
                return numpy.frombuffer(data, dtype=numpy.float32).copy()
            except ImportError:
                return _array.array('f', data)

        def _to_vector(v, alloc):
            try:
                import numpy
                return numpy.asarray(v, dtype=numpy.float32).tobytes()
            except ImportError:
                return _array.array('f', v).tobytes()

        def _to_bytes(v, alloc):
            if isinstance(v, str):
                return v.encode('utf-8')
            return bytes(v)

        def _time(us):
            return _dt.timedelta(microseconds=us)

        def _load(src, name, entry, mle):
            ns = {'__name__': name, 'Mle': mle}
            exec(compile(src, name + '.py', 'exec'), ns)
            return ns[entry]

        _mle_helpers = {
            'bytes': lambda b: bytes(b),
            'vector': _vector,
            'json': lambda s: _json.loads(s),
            'date': lambda s: _dt.date.fromisoformat(s),
            'datetime': lambda s: _dt.datetime.fromisoformat(s),
            'time': _time,
            'decimal': lambda s: _decimal.Decimal(s),
            'bigint': lambda s: int(s),
            'str': lambda v: str(v),
            'toJson': lambda v: _json.dumps(v, separators=(',', ':')),
            'toBytes': _to_bytes,
            'toVector': _to_vector,
            'toNumber': lambda v: float(v),
            'loader': _load,
        }
        _mle_helpers
        """));
    return new Helpers(ns.getHashValue("bytes"), ns.getHashValue("vector"), ns.getHashValue("json"),
        ns.getHashValue("date"), ns.getHashValue("datetime"), ns.getHashValue("time"),
        ns.getHashValue("decimal"), ns.getHashValue("bigint"), ns.getHashValue("str"),
        ns.getHashValue("toJson"), ns.getHashValue("toBytes"), ns.getHashValue("toVector"),
        ns.getHashValue("toNumber"), ns.getHashValue("loader"));
  }

  private Helpers helpers(String language) {
    return language.equals("js") ? js : python;
  }

  // ------------------------------------------------------------ routines

  private static final Pattern IDENTIFIER = Pattern.compile("[A-Za-z_$][A-Za-z0-9_$]*");

  Routine get(long id) {
    return routines.get(id);
  }

  void drop(long id) {
    routines.remove(id);
  }

  void reset() {
    routines.clear();
  }

  /** Loads javac-free: evaluates the source and finds the entry function. */
  void prepare(long id, String language, String entry, String source, List<JavaRoutines.Param> params,
      Values.ParamType ret) throws JavaRoutines.CompileException {
    if (!IDENTIFIER.matcher(entry).matches())
      throw new JavaRoutines.CompileException("the entry must be a function name, not '" + entry + "'");
    Context c = context();
    Helpers h = helpers(language);
    String name = "routine_" + id;
    Value fn;
    try {
      if (language.equals("js")) {
        // Same first line, so that positions in errors match the source.
        String wrapped = "(function(){" + source + "\n;return (typeof " + entry + " === 'function') ? " + entry
            + " : undefined;})()";
        fn = c.eval(Source.newBuilder("js", wrapped, name + ".js").buildLiteral());
      } else {
        fn = h.loader().execute(source, name, entry, mleApi);
      }
    } catch (PolyglotException e) {
      // KILL and exhaustion while loading are not compile errors.
      if (e.isInterrupted() || e.isCancelled() || e.isResourceExhausted()
          || (e.isHostException() && e.asHostException() instanceof Error))
        throw e;
      throw new JavaRoutines.CompileException(describe(e, true));
    }
    if (fn == null || !fn.canExecute())
      throw new JavaRoutines.CompileException("the source does not define a function " + entry);
    Value[] converters = new Value[params.size()];
    for (int i = 0; i < converters.length; i++) converters[i] = converter(h, params.get(i).type().tag(), language);
    routines.put(id, new Routine(language, fn, converters, ret));
  }

  private static Value converter(Helpers h, int tag, String language) {
    return switch (tag) {
      case Wire.T_BYTES -> h.bytes();
      case Wire.T_VECTOR -> h.vector();
      case Wire.T_JSON -> h.json();
      case Wire.T_DATE -> h.date();
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> h.datetime();
      case Wire.T_TIME -> h.time();
      case Wire.T_DECIMAL -> h.decimal();
      case Wire.T_I64, Wire.T_U64 -> null;  // big values come as strings: see hostForm
      default -> null;
    };
  }

  /** Calls the routine; returns the result as a Java value for {@link Values#writeAs}. */
  Object invoke(Routine r, Object[] neutral) {
    Object[] args = new Object[neutral.length];
    boolean js = r.language().equals("js");
    for (int i = 0; i < args.length; i++) {
      Object v = hostForm(neutral[i], js);
      args[i] = v == null || r.converters()[i] == null ? v : r.converters()[i].execute(v);
    }
    Value result = r.fn().execute(args);
    return r.ret() == null ? null : toJava(result, r.ret(), helpers(r.language()));
  }

  /** The neutral value in the form the language's converter takes. */
  private static Object hostForm(Object v, boolean js) {
    return switch (v) {
      case null -> null;
      // A JS number; beyond 2^53 a string (Oracle's UNSAFE_STRING default). Passed as a Long,
      // so that integers stay integers when a routine passes them back to SQL.
      case Long l -> js && Math.abs(l) > (1L << 53) - 1 ? l.toString() : (Object) l;
      case BigInteger b -> js ? b.toString() : (Object) b;
      case BigDecimal d -> d.toPlainString();
      case MleJson j -> j.text();
      case byte[] b -> ByteBuffer.wrap(b);
      case Values.VectorRef ref -> {
        ByteBuffer bb = ByteBuffer.allocate(ref.dims() * 4).order(ByteOrder.LITTLE_ENDIAN);
        java.lang.foreign.MemorySegment.ofBuffer(bb).copyFrom(ref.data());
        yield bb;
      }
      case Values.Temporal t -> switch (t.tag()) {
        case Wire.T_TIME -> {
          long us = ((t.hour() * 60 + t.minute()) * 60 + t.second()) * 1_000_000L + t.micro();
          yield js ? formatTime(t) : (Object) (t.negative() ? -us : us);
        }
        case Wire.T_DATE -> js
            ? (Object) (double) ((LocalDate) t.toJava()).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
            : t.toJava().toString();
        default -> js
            ? (Object) (double) ((LocalDateTime) t.toJava()).toInstant(ZoneOffset.UTC).toEpochMilli()
            : t.toJava().toString();
      };
      default -> v;  // Double, Float, String
    };
  }

  private static String formatTime(Values.Temporal t) {
    return String.format("%s%02d:%02d:%02d%s", t.negative() ? "-" : "", t.hour(), t.minute(), t.second(),
        t.micro() == 0 ? "" : String.format(".%06d", t.micro()));
  }

  private static final Pattern TIME = Pattern.compile("(-)?(\\d+):(\\d{2}):(\\d{2})(?:\\.(\\d{1,6}))?");

  /** A guest result as a Java value of the declared SQL type. */
  private static Object toJava(Value v, Values.ParamType t, Helpers h) {
    if (v == null || v.isNull()) return null;
    switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        if (v.fitsInLong()) return v.asLong();
        if (v.fitsInBigInteger()) return v.asBigInteger();
        if (v.fitsInDouble()) return v.asDouble();
        if (v.isString()) return parseDecimal(v.asString(), t);
        return parseDecimal(h.str().execute(v).asString(), t);
      }
      case Wire.T_F64, Wire.T_F32 -> {
        if (v.fitsInDouble()) return v.asDouble();
        return h.toNumber().execute(v).asDouble();
      }
      case Wire.T_DECIMAL -> {
        if (v.fitsInLong()) return v.asLong();
        if (v.fitsInDouble()) return v.asDouble();
        return parseDecimal(v.isString() ? v.asString() : h.str().execute(v).asString(), t);
      }
      case Wire.T_STRING -> {
        return v.isString() ? v.asString() : h.str().execute(v).asString();
      }
      case Wire.T_JSON -> {
        return new MleJson(h.toJson().execute(v).asString());
      }
      case Wire.T_BYTES -> {
        if (v.isString()) return v.asString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
        return bytesOf(v.hasBufferElements() ? v : h.toBytes().execute(v, ALLOC));
      }
      case Wire.T_VECTOR -> {
        ByteBuffer b = ByteBuffer.wrap(bytesOf(h.toVector().execute(v, ALLOC))).order(ByteOrder.LITTLE_ENDIAN);
        float[] f = new float[b.remaining() / 4];
        b.asFloatBuffer().get(f);
        return f;
      }
      case Wire.T_DATE -> {
        if (v.isDate() && !v.isTime()) return v.asDate();
        if (v.isInstant()) return LocalDate.ofInstant(v.asInstant(), ZoneOffset.UTC);
        if (v.isDate()) return v.asDate();
        throw new Values.ConversionException("cannot convert " + v + " to DATE");
      }
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> {
        if (v.isDate() && v.isTime() && !v.isTimeZone()) return LocalDateTime.of(v.asDate(), v.asTime());
        if (v.isInstant()) return LocalDateTime.ofInstant(v.asInstant(), ZoneOffset.UTC);
        if (v.isDate()) return v.asDate();
        throw new Values.ConversionException("cannot convert " + v + " to " + t.sqlName());
      }
      case Wire.T_TIME -> {
        if (v.isDuration()) return v.asDuration();
        if (v.isTime()) return v.asTime();
        String s = v.isString() ? v.asString() : h.str().execute(v).asString();
        Matcher m = TIME.matcher(s);
        if (!m.matches()) throw new Values.ConversionException("cannot convert '" + s + "' to TIME");
        Duration d = Duration.ofHours(Long.parseLong(m.group(2))).plusMinutes(Long.parseLong(m.group(3)))
            .plusSeconds(Long.parseLong(m.group(4)));
        if (m.group(5) != null)
          d = d.plusNanos(Long.parseLong((m.group(5) + "000000").substring(0, 6)) * 1000);
        return m.group(1) != null ? d.negated() : d;
      }
      default -> throw new Values.ConversionException("cannot convert to " + t.sqlName());
    }
  }

  private static Object parseDecimal(String s, Values.ParamType t) {
    try {
      return new BigDecimal(s.trim());
    } catch (NumberFormatException e) {
      throw new Values.ConversionException("cannot convert '" + s + "' to " + t.sqlName());
    }
  }

  private static final ProxyExecutable ALLOC = a -> ByteBuffer.allocate(a[0].asInt());

  private static byte[] bytesOf(Value v) {
    if (v.isHostObject() && v.asHostObject() instanceof ByteBuffer bb) {
      byte[] b = new byte[bb.remaining()];
      bb.duplicate().get(b);
      return b;
    }
    if (v.hasBufferElements()) {
      byte[] b = new byte[Math.toIntExact(v.getBufferSize())];
      v.readBuffer(0, b, 0, b.length);
      return b;
    }
    throw new Values.ConversionException("not a byte sequence: " + v);
  }

  // ------------------------------------------------------------ errors

  /** "routine_7.js:3:5: SyntaxError: ..." for syntax errors, else the message. */
  static String describe(PolyglotException e, boolean withLocation) {
    String msg = e.getMessage() == null ? e.toString() : e.getMessage();
    if (withLocation && e.isSyntaxError() && e.getSourceLocation() != null) {
      var loc = e.getSourceLocation();
      return loc.getSource().getName() + ":" + loc.getStartLine() + ":" + loc.getStartColumn() + ": " + msg;
    }
    return msg;
  }

  /** The guest frames of e (up to the protocol's limit). */
  static List<Dispatcher.Frame> frames(PolyglotException e) {
    List<Dispatcher.Frame> frames = new java.util.ArrayList<>();
    for (PolyglotException.StackFrame f : e.getPolyglotStackTrace()) {
      if (!f.isGuestFrame() || f.getSourceLocation() == null) continue;
      var loc = f.getSourceLocation();
      String name = loc.getSource().getName();
      if (!name.startsWith("routine_")) continue;  // the worker's own helpers
      frames.add(new Dispatcher.Frame(f.getRootName() == null ? "" : f.getRootName(), name, loc.getStartLine(),
          loc.getStartColumn()));
      if (frames.size() == Wire.MAX_STACK_FRAMES) break;
    }
    return frames;
  }

  static String languageOf(PolyglotException e) {
    for (PolyglotException.StackFrame f : e.getPolyglotStackTrace())
      if (f.isGuestFrame() && f.getLanguage() != null) return f.getLanguage().getId();
    return "";
  }
}
