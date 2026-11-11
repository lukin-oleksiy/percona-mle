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

  /** Phase 7: guest code cannot start threads through Java either. */
  private static final java.util.Set<String> THREAD_CLASSES = java.util.Set.of("java.lang.Thread",
      "java.lang.ThreadGroup", "java.lang.Thread$Builder", "java.util.concurrent.Executors",
      "java.util.concurrent.ThreadPoolExecutor", "java.util.concurrent.ScheduledThreadPoolExecutor",
      "java.util.concurrent.ForkJoinPool", "java.util.concurrent.CompletableFuture", "java.util.Timer");

  /**
   * A prepared routine. Body form (entry ""): js is set for JavaScript; Python routines with OUT
   * parameters return (result, out1, ...), outs gives the parameters' indexes.
   */
  record Routine(String language, Value fn, Value[] converters, Values.ParamType ret, Values.ParamType[] params,
      int[] outs, JsOracle.Routine js, List<LibraryFileSystem.Import> imports) {
    int arity() {
      return params.length;
    }
  }

  /** Scheme of the URIs of routine sources (stack frames are kept only for these). */
  static final String ROUTINE_SCHEME = "mle-routine";
  /**
   * Routine sources are file URIs below this (virtual, never read) directory, so that module paths
   * in them ("/schema/library") resolve to files of LibraryFileSystem (Phase 6).
   */
  static final String ROUTINE_DIR = "/.mle-routines/";

  static java.net.URI routineUri(long id) {
    return java.net.URI.create("file://" + ROUTINE_DIR + id + ".js");
  }

  static boolean isRoutineUri(java.net.URI uri) {
    return uri != null && ("file".equals(uri.getScheme()) && uri.getPath() != null
        && uri.getPath().startsWith(ROUTINE_DIR) || ROUTINE_SCHEME.equals(uri.getScheme()));
  }

  private final Console console;
  private final Options options;
  private final Map<Long, Routine> routines = new HashMap<>();
  /** Source names of the Python routines (Python sources have no URI of ours). */
  private static final java.util.Set<String> pythonSources = java.util.concurrent.ConcurrentHashMap.newKeySet();
  /** Length of the wrapper text before the body on line 1, by JS routine source URI. */
  static final Map<java.net.URI, Integer> prefixes = new java.util.concurrent.ConcurrentHashMap<>();
  private Value jsOracle;
  private PySql pySql;
  private Value jsImport;
  private volatile Context context;
  private Helpers js;
  private Helpers python;
  private Value mleApi;

  /** Per language: argument converters and result extractors written in the language. */
  private record Helpers(Value bytes, Value vector, Value json, Value date, Value datetime, Value time,
      Value decimal, Value bigint, Value str, Value toJson, Value toBytes, Value toVector, Value toNumber,
      Value loader) {}

  Polyglot(Console console, Options options) {
    this.console = console;
    this.options = options;
  }

  /** OPTIONS: the JavaScript converters read the integer and decimal types from $o. */
  void applyOptions() {
    if (jsOracle == null) return;
    Value o = jsOracle.getMember("$o");
    o.putMember("int", options.integerType);
    o.putMember("dec", options.decimalType);
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
      // The static side of com.percona.mle.api.Mle (Mle.context(), Mle.session()) in JS and Python.
      mleApi = c.asValue(com.percona.mle.api.Mle.class).getMember("static");
      c.getBindings("js").putMember("Mle", mleApi);
      c.getPolyglotBindings().putMember("Mle", mleApi);
      js = jsHelpers(c);
      jsOracle = JsOracle.helpers(c);
      // USING in JavaScript routines (Phase 6): $c.lib(...) asks for non-JavaScript libraries.
      jsOracle.putMember("libBridge",
          (org.graalvm.polyglot.proxy.ProxyExecutable) a -> libraryModule(a[0].asString(), a[1].asString(), true));
      jsImport = c.eval("js", "(p) => import(p)");
      // importPython('numpy'): Python modules from JavaScript.
      c.getBindings("js").putMember("importPython",
          (org.graalvm.polyglot.proxy.ProxyExecutable) a -> importPython(a[0].asString()));
      // The SQL API (Phase 5): the globals session, mysql and SqlError. Python's are made
      // with Python's helpers, on first use (GraalPy costs ~150 MB: JS-only workers skip it).
      new JsSql(c, options);
      // Oracle: console.clear() clears the session's stdout and stderr.
      if (console != null) {
        Value clear = c.asValue((org.graalvm.polyglot.proxy.ProxyExecutable) a -> {
          console.clear();
          return null;
        });
        c.eval("js", "(f) => { console.clear = () => { f(); }; }").execute(clear);
      }
      context = c;
      applyOptions();
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
        .allowHostClassLookup(name -> !name.startsWith("com.percona.mle.worker.") && !THREAD_CLASSES.contains(name))
        // The DBA's jars (Phase 6) for Java.type() in JavaScript and java.type() in Python.
        .hostClassLoader(JavaRoutines.hostLoader())
        .allowPolyglotAccess(PolyglotAccess.ALL)
        .allowCreateThread(true)  // GraalPy's C API helper thread
        .allowCreateProcess(false)
        .allowNativeAccess(true)  // numpy and other native extensions
        .allowEnvironmentAccess(EnvironmentAccess.NONE)
        .allowIO(IOAccess.newBuilder()
            // Database libraries (Phase 6) are modules at "/schema/library".
            .fileSystem(new LibraryFileSystem(FileSystem.allowInternalResourceAccess(FileSystem.allowLanguageHomeAccess(
                FileSystem.newReadOnlyFileSystem(FileSystem.newDefaultFileSystem())))))
            .allowHostSocketAccess(false)
            .build())
        .option("engine.WarnInterpreterOnly", "false")
        .option(FrameLimit.ID, "true")
        .option("python.PosixModuleBackend", "java")
        .option("python.DontWriteBytecodeFlag", "true")
        .option("python.NoUserSiteFlag", "true")
        .option("python.WarnExperimentalFeatures", "false")
        .environment("HOME", "/nonexistent");  // GraalPy looks up the home directory (U-1)
    // Third-party packages installed by the DBA (14 §3): npm packages for require() and
    // import of bare names (CommonJS support is an experimental GraalJS option), Python
    // packages on the module path.
    String packages = System.getProperty("mle.packages", "");
    if (!packages.isEmpty()) {
      Path js = Path.of(packages, "js");
      if (java.nio.file.Files.isDirectory(js.resolve("node_modules")))
        b.allowExperimentalOptions(true)
            .option("js.commonjs-require", "true")
            .option("js.commonjs-require-cwd", js.toString())
            .option("js.esm-bare-specifier-relative-lookup", "true");
      Path site = Path.of(packages, "python", "site-packages");
      if (java.nio.file.Files.isDirectory(site)) b.option("python.PythonPath", site.toString());
    }
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

        # Phase 7: routines cannot start threads (deterministic KILL and memory accounting).
        import _thread as _mle_thread_mod, threading as _mle_threading, types as _mle_types_mod
        def _mle_no_threads(*args, **kwargs):
            raise RuntimeError('threads are not available in MLE routines')
        _mle_thread_mod.start_new_thread = _mle_no_threads
        if hasattr(_mle_thread_mod, 'start_new'):
            _mle_thread_mod.start_new = _mle_no_threads
        if hasattr(_mle_thread_mod, 'start_joinable_thread'):
            _mle_thread_mod.start_joinable_thread = _mle_no_threads
        _mle_threading.Thread.start = lambda self: _mle_no_threads()

        # Phase 7 (S-14): numpy arrays the host fills with one memcpy; creating numpy arrays is
        # the expensive part, so argument arrays are reused (read-only, valid during the call).
        def _numpy_buffer(shape, readonly):
            try:
                import numpy
            except ImportError:
                return None
            a = numpy.empty(shape, dtype=numpy.float32)
            if readonly:
                a.setflags(write=False)
            return (a, a.__array_interface__['data'][0])

        def _numpy_matrix(n, d):
            import numpy
            a = numpy.empty((n, d), dtype=numpy.float32)
            return (a, a.__array_interface__['data'][0])

        def _numpy_numbers(shape, kind):
            import numpy
            a = numpy.empty(shape, dtype=numpy.int64 if kind == 'i' else numpy.float64)
            return (a, a.__array_interface__['data'][0])

        def _vector(b):
            data = b.encode('latin-1') if isinstance(b, str) else bytes(b)
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

        # The SQL API's globals (session, mysql, SqlError), filled by PySql.
        _mle_globals = {}

        def _load(src, name, entry, mle):
            ns = {'__name__': name, 'Mle': mle}
            ns.update(_mle_globals)
            exec(compile(src, name + '.py', 'exec'), ns)
            return ns[entry]

        def _load_body(src, name, params, has_outs, mle):
            # The body becomes the function body; ast keeps its line numbers. With OUT
            # parameters it runs in an inner function (it may return early) that declares all
            # parameters nonlocal, and the function returns (result, *parameters).
            # Bodies are usually indented inside $$ ... $$ (or start after
            # "$$ "): remove the common indentation (lines stay where they are).
            import ast, textwrap
            tree = ast.parse(textwrap.dedent(src), filename=name)
            def fn(fname, args, body):
                kw = dict(name=fname, args=ast.arguments(posonlyargs=[], args=[ast.arg(arg=a) for a in args],
                          vararg=None, kwonlyargs=[], kw_defaults=[], kwarg=None, defaults=[]),
                          body=body or [ast.Pass()], decorator_list=[], returns=None)
                if 'type_params' in ast.FunctionDef._fields:
                    kw['type_params'] = []
                return ast.FunctionDef(**kw)
            body = tree.body
            if has_outs:
                inner = fn('__mle_body', [], [ast.Nonlocal(names=list(params))] + body)
                call = ast.Assign(targets=[ast.Name(id='__mle_r', ctx=ast.Store())],
                                  value=ast.Call(func=ast.Name(id='__mle_body', ctx=ast.Load()), args=[], keywords=[]))
                ret = ast.Return(value=ast.Tuple(elts=[ast.Name(id='__mle_r', ctx=ast.Load())]
                                 + [ast.Name(id=p, ctx=ast.Load()) for p in params], ctx=ast.Load()))
                body = [inner, call, ret]
            mod = ast.Module(body=[fn('__mle_routine', list(params), body)], type_ignores=[])
            ast.fix_missing_locations(mod)
            ns = {'__name__': name, 'Mle': mle}
            ns.update(_mle_globals)
            exec(compile(mod, name, 'exec'), ns)
            # Phase 7: fresh globals per call; state belongs in libraries (15 §1). Each call runs
            # the routine's code with a copy of the prepared globals.
            code, base, make = ns['__mle_routine'].__code__, ns, _mle_types_mod.FunctionType
            def fresh(*args):
                return make(code, base.copy())(*args)
            fresh.__mle_base__ = base
            return fresh

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
            'body_loader': _load_body,
            'numpy_buffer': _numpy_buffer,
            'numpy_numbers': _numpy_numbers,
            'numpy_matrix': _numpy_matrix,
            'dict': lambda: {},
        }
        _mle_helpers
        """));
    return new Helpers(ns.getHashValue("bytes"), ns.getHashValue("vector"), ns.getHashValue("json"),
        ns.getHashValue("date"), ns.getHashValue("datetime"), ns.getHashValue("time"),
        ns.getHashValue("decimal"), ns.getHashValue("bigint"), ns.getHashValue("str"),
        ns.getHashValue("toJson"), ns.getHashValue("toBytes"), ns.getHashValue("toVector"),
        ns.getHashValue("toNumber"), ns.getHashValue("loader"));
  }

  private Value pythonBodyLoader() {
    pythonReady();
    return context.getBindings("python").getMember("_mle_helpers").getHashValue("body_loader");
  }

  /**
   * A database library's module for a routine of another language (Phase 6): a Python library as a
   * Python module (evaluated once per version); a JavaScript library as its module namespace, or
   * null for JavaScript routines (they import it themselves). Java libraries are for Java routines.
   */
  Object libraryModule(String schema, String name, boolean forJs) {
    LibraryFileSystem.Library l;
    try {
      l = LibraryFileSystem.library(schema, name);
    } catch (java.io.IOException e) {
      throw new IllegalArgumentException(e.getMessage() == null ? e.toString() : e.getMessage());
    }
    switch (l.language()) {
      case "js" -> {
        if (forJs) return null;
        Value settled = jsOracle.getMember("settle").execute(jsImport.execute("/" + schema + "/" + name));
        if (!settled.getMember("done").asBoolean())
          throw new IllegalStateException("library " + schema + "." + name + " did not load");
        if (!settled.getMember("ok").asBoolean()) jsOracle.getMember("rethrow").execute(settled.getMember("v"));
        return settled.getMember("v");
      }
      case "python" -> {
        pythonReady();
        return pySql.loadLibrary.execute(schema + "." + name, l.hash(),
            new String(l.body(), java.nio.charset.StandardCharsets.UTF_8));
      }
      default -> {
        // A Java library: compiled once per version into its own class loader; the namespace's
        // members are its classes (static side: static methods, constructors with new).
        return javaLibrary(schema + "." + name, l);
      }
    }
  }

  private final Map<String, Object[]> javaLibraries = new HashMap<>();

  private Object javaLibrary(String key, LibraryFileSystem.Library l) {
    Object[] hit = javaLibraries.get(key);
    if (hit != null && (long) hit[0] == l.hash()) return hit[1];
    Map<String, Class<?>> classes;
    try {
      classes = JavaRoutines.loadLibrary(key, new String(l.body(), java.nio.charset.StandardCharsets.UTF_8));
    } catch (JavaRoutines.CompileException e) {
      throw new IllegalArgumentException("Java library " + key + ": " + e.getMessage());
    }
    Context c = context();
    Map<String, Object> members = new HashMap<>();
    classes.forEach((simple, cls) -> members.put(simple, c.asValue(cls).getMember("static")));
    Object ns = org.graalvm.polyglot.proxy.ProxyObject.fromMap(members);
    javaLibraries.put(key, new Object[] {l.hash(), ns});
    return ns;
  }

  /** importPython(name) / Mle.importPython: a Python module (numpy ...) for any language. */
  Value importPython(String module) {
    Context c = context();
    return c.eval("python", "import importlib\nlambda n: importlib.import_module(n)").execute(module);
  }

  /**
   * import_js(name) / Mle.importJs: an npm package (Phase 6) for any language: require() for
   * CommonJS, else import() of an ES module.
   */
  Value importJs(String name) {
    Context c = context();
    Value required = c.eval("js", "(n) => { try { return require(n) } catch (e) { return null } }").execute(name);
    if (!required.isNull()) return required;
    Value settled = jsOracle.getMember("settle").execute(jsImport.execute(name));
    if (!settled.getMember("done").asBoolean())
      throw new IllegalStateException("module " + name + " did not load");
    if (!settled.getMember("ok").asBoolean()) jsOracle.getMember("rethrow").execute(settled.getMember("v"));
    return settled.getMember("v");
  }

  /** Python's helpers and SQL API, made when the first Python routine is loaded. */
  private void pythonReady() {
    if (python != null) return;
    Context c = context();
    // All or nothing: an interrupt (KILL) in between leaves Python to be set up again.
    Helpers h = pythonHelpers(c);
    pySql = new PySql(c, options, a -> libraryModule(a[0].asString(), a[1].asString(), false),
        a -> importJs(a[0].asString()));
    python = h;
  }

  private Helpers helpers(String language) {
    if (language.equals("js")) return js;
    pythonReady();
    return python;
  }

  // ------------------------------------------------------------ routines

  private static final Pattern IDENTIFIER = Pattern.compile("[A-Za-z_$][A-Za-z0-9_$]*");

  Routine get(long id) {
    return routines.get(id);
  }

  void drop(long id) {
    routines.remove(id);
    prefixes.remove(routineUri(id));
  }

  void reset() {
    routines.clear();
    prefixes.clear();
    pythonSources.clear();
  }

  /**
   * Loads a routine. With an entry, the source defines a function of that name (Phase 3 form);
   * with an empty entry, the source is the routine body (JavaScript: Oracle's form, JsOracle).
   */
  void prepare(long id, String language, String routineName, String entry, String source,
      List<JavaRoutines.Param> params, Values.ParamType ret, List<LibraryFileSystem.Import> imports)
      throws JavaRoutines.CompileException {
    boolean bodyForm = entry.isEmpty();
    if (!bodyForm && !IDENTIFIER.matcher(entry).matches())
      throw new JavaRoutines.CompileException("the entry must be a function name, not '" + entry + "'");
    Context c = context();
    Helpers h = helpers(language);
    String name = routineName.isEmpty() ? "routine_" + id : routineName;
    java.net.URI uri = routineUri(id);
    Values.ParamType[] types = new Values.ParamType[params.size()];
    for (int i = 0; i < types.length; i++) types[i] = params.get(i).type();
    int[] outs = java.util.stream.IntStream.range(0, params.size())
        .filter(i -> params.get(i).mode() != Wire.MODE_IN).toArray();
    Value fn;
    JsOracle.Routine jsRoutine = null;
    try {
      if (language.equals("js") && bodyForm) {
        for (JavaRoutines.Param p : params)
          if (!IDENTIFIER.matcher(p.name()).matches())
            throw new JavaRoutines.CompileException("'" + p.name() + "' is not a JavaScript identifier");
        jsRoutine = JsOracle.prepare(c, jsOracle, name, source, params, ret, uri, imports);
        fn = jsRoutine.fn();
      } else if (language.equals("js")) {
        // Same first line, so that positions in errors match the source.
        String wrapped = "(function(){" + source + "\n;return (typeof " + entry + " === 'function') ? " + entry
            + " : undefined;})()";
        prefixes.put(uri, "(function(){".length());
        fn = c.eval(Source.newBuilder("js", wrapped, name + ".js").uri(uri).buildLiteral());
      } else if (bodyForm) {
        pythonSources.add(name);
        String[] names = params.stream().map(JavaRoutines.Param::name).toArray(String[]::new);
        fn = pythonBodyLoader().execute(source, name, names, outs.length > 0, mleApi);
      } else {
        pythonSources.add(name + ".py");
        fn = h.loader().execute(source, name, entry, mleApi);
      }
    } catch (PolyglotException e) {
      // KILL and exhaustion while loading are not compile errors.
      if (e.isInterrupted() || e.isCancelled() || e.isResourceExhausted()
          || (e.isHostException() && e.asHostException() instanceof Error))
        throw e;
      throw new JavaRoutines.CompileException(bodyForm ? bodyCompileError(e, uri) : describe(e, true));
    }
    if (fn == null || !fn.canExecute())
      throw new JavaRoutines.CompileException("the source does not define a function " + entry);
    Value[] converters = new Value[params.size()];
    if (jsRoutine == null)
      for (int i = 0; i < converters.length; i++) converters[i] = converter(h, types[i].tag(), language);
    routines.put(id, new Routine(language, fn, converters, ret, types, outs, jsRoutine, imports));
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

  /**
   * Calls the routine; returns the result as a Java value for {@link Values#writeAs} and puts the
   * OUT and INOUT values into outs (by parameter index).
   */
  Object invoke(Routine r, Object[] neutral, Object[] outs) {
    if (r.js() != null) return invokeJs(r, neutral, outs);
    Object[] args = new Object[neutral.length];
    boolean js = r.language().equals("js");
    for (int i = 0; i < args.length; i++) {
      if (!js && neutral[i] instanceof Values.VectorRef ref) {
        Object a = vectorArgument(i, ref);
        if (a != null) {
          args[i] = a;
          continue;
        }
      }
      Object v = hostForm(neutral[i], js);
      args[i] = v == null || r.converters()[i] == null ? v : r.converters()[i].execute(v);
    }
    // USING (Phase 6): the libraries' current modules, bound before each call.
    if (r.language().equals("python"))
      for (LibraryFileSystem.Import i : r.imports())
        pySql.bind.execute(r.fn(), i.binding(), libraryModule(i.schema(), i.name(), false));
    Value result = r.fn().execute(args);
    Helpers h = helpers(r.language());
    if (r.outs().length > 0) {
      // Python body form: (result, parameter 1, ...)
      for (int i : r.outs()) outs[i] = toJava(result.getArrayElement(i + 1), r.params()[i], h);
      result = result.getArrayElement(0);
    }
    return r.ret() == null ? null : toJava(result, r.ret(), h);
  }

  private Object invokeJs(Routine r, Object[] neutral, Object[] outs) {
    JsOracle.Routine j = r.js();
    Object[] args = new Object[neutral.length];
    for (int i = 0; i < args.length; i++) args[i] = JsOracle.hostForm(neutral[i], j.params()[i], options);
    Value result = j.fn().execute(args);
    if (j.async()) {
      Value settled = jsOracle.getMember("settle").execute(result);
      if (!settled.getMember("done").asBoolean())
        throw new Values.ConversionException("the routine's promise did not settle");
      result = settled.getMember("v");
      if (!settled.getMember("ok").asBoolean()) jsOracle.getMember("rethrow").execute(result);
    }
    if (j.outs().length > 0) {
      for (int k = 0; k < j.outs().length; k++) {
        int i = j.outs()[k];
        outs[i] = JsOracle.toJava(result.getArrayElement(k + 1), j.params()[i], options, jsOracle);
      }
      result = result.getArrayElement(0);
    }
    return j.ret() == null ? null : JsOracle.toJava(result, j.ret(), options, jsOracle);
  }

  /** Reused read-only numpy arrays for VECTOR arguments, by nesting depth, parameter and dims. */
  private final Map<String, Object[]> vectorBuffers = new HashMap<>();

  /**
   * A VECTOR argument for Python (Phase 7, S-14): a numpy array reused per parameter, dims and
   * nesting depth, filled with one copy; read-only and valid during the call (copy() keeps it).
   * Null without numpy (then array.array, as before).
   */
  @SuppressWarnings("restricted")  // the array's own memory, as numpy reports it
  private Object vectorArgument(int index, Values.VectorRef ref) {
    int depth = SqlBridge.dispatcher == null ? 0 : SqlBridge.dispatcher.depth();
    String key = depth + ":" + index + ":" + ref.dims();
    Object[] buf = vectorBuffers.get(key);
    if (buf == null) {
      Value t = pythonHelper("numpy_buffer").execute(ref.dims(), true);
      if (t.isNull()) return null;
      buf = new Object[] {t.getArrayElement(0),
          java.lang.foreign.MemorySegment.ofAddress(t.getArrayElement(1).asLong()).reinterpret(ref.dims() * 4L)};
      vectorBuffers.put(key, buf);
    }
    ((java.lang.foreign.MemorySegment) buf[1]).copyFrom(ref.data());
    return buf[0];
  }

  Value pythonHelper(String name) {
    pythonReady();
    return context.getBindings("python").getMember("_mle_helpers").getHashValue(name);
  }

  /** Pre-warming (mle.pool_prewarm): Python, and numpy too. */
  void prewarmPython(boolean numpy) {
    pythonReady();
    if (numpy) context().eval("python", "import numpy");
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
        if (!js) {
          // Python: a Latin-1 string (one byte per char, stored compactly by Truffle) turns into
          // bytes natively in GraalPy; bytes() of a foreign buffer goes element by element.
          byte[] raw = ref.data().toArray(java.lang.foreign.ValueLayout.JAVA_BYTE);
          yield new String(raw, java.nio.charset.StandardCharsets.ISO_8859_1);
        }
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
        return parseTime(v.isString() ? v.asString() : h.str().execute(v).asString(), t);
      }
      default -> throw new Values.ConversionException("cannot convert to " + t.sqlName());
    }
  }

  /** [-]h:mm:ss[.ffffff] as a Duration. */
  static Duration parseTime(String s, Values.ParamType t) {
    Matcher m = TIME.matcher(s.trim());
    if (!m.matches()) throw new Values.ConversionException("Cannot convert value '" + s + "' to TIME from MLE");
    Duration d = Duration.ofHours(Long.parseLong(m.group(2))).plusMinutes(Long.parseLong(m.group(3)))
        .plusSeconds(Long.parseLong(m.group(4)));
    if (m.group(5) != null)
      d = d.plusNanos(Long.parseLong((m.group(5) + "000000").substring(0, 6)) * 1000);
    return m.group(1) != null ? d.negated() : d;
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

  /**
   * CREATE LIBRARY: checks a library body without running it; the syntax error (Oracle's form,
   * "SyntaxError: name:line:col message") or null.
   */
  String parseLibrary(String language, String name, String body) {
    Context c = context();
    try {
      switch (language) {
        case "js" -> c.parse(Source.newBuilder("js", body, name).mimeType("application/javascript+module")
            .buildLiteral());
        case "python" -> {
          pythonReady();
          c.eval("python", "compile").execute(body, name, "exec");
        }
        default -> {
          return "libraries in " + language + " are not supported";
        }
      }
      return null;
    } catch (PolyglotException e) {
      if (e.isInterrupted() || e.isCancelled() || e.isResourceExhausted()) throw e;
      String m = e.getMessage() == null ? e.toString() : e.getMessage();
      int nl = m.indexOf('\n');
      return nl < 0 ? m : m.substring(0, nl);
    }
  }

  /** "routine_7.js:3:5: SyntaxError: ..." for syntax errors, else the message. */
  static String describe(PolyglotException e, boolean withLocation) {
    String msg = e.getMessage() == null ? e.toString() : e.getMessage();
    if (withLocation && e.isSyntaxError() && e.getSourceLocation() != null) {
      var loc = e.getSourceLocation();
      var src = loc.getSource();
      int line = loc.getStartLine();
      return src.getName() + ":" + line + ":" + bodyColumn(src, line, loc.getStartColumn()) + ": " + msg;
    }
    return msg;
  }

  private static final Pattern LOCATION_LINE_1 = Pattern.compile("^(.*?:1:)(\\d+)( .*)$");

  /**
   * A body's compile error as the language reports it: GraalJS gives Oracle's form
   * ("SyntaxError: name:line:col message", the source line, a caret), Python names the line. On
   * the body's first line the JavaScript wrapper text before the body is taken out of the column,
   * the source line and the caret.
   */
  static String bodyCompileError(PolyglotException e, java.net.URI uri) {
    String msg = e.getMessage() == null ? e.toString() : e.getMessage();
    Integer prefix = prefixes.get(uri);
    if (prefix == null || e.getSourceLocation() == null || e.getSourceLocation().getStartLine() != 1) return msg;
    String[] lines = msg.split("\n", -1);
    Matcher m = LOCATION_LINE_1.matcher(lines[0]);
    if (m.matches())
      lines[0] = m.group(1) + Math.max(0, Integer.parseInt(m.group(2)) - prefix) + m.group(3);
    if (lines.length >= 3 && lines[1].length() >= prefix && lines[2].length() >= prefix) {
      lines[1] = lines[1].substring(prefix);
      lines[2] = lines[2].substring(prefix);
    }
    return String.join("\n", lines);
  }

  /** Whether a source is a routine's (not the worker's own helpers). */
  static boolean isRoutineSource(org.graalvm.polyglot.Source src) {
    java.net.URI uri = src.getURI();
    return isRoutineUri(uri) || pythonSources.contains(src.getName());
  }

  /** Column on the body's first line: the wrapper text before the body is not counted. */
  static int bodyColumn(org.graalvm.polyglot.Source src, int line, int column) {
    if (line != 1 || src.getURI() == null) return column;
    Integer prefix = prefixes.get(src.getURI());
    return prefix == null ? column : Math.max(1, column - prefix);
  }

  /** The guest frames of e (up to the protocol's limit), innermost first. */
  static List<Dispatcher.Frame> frames(PolyglotException e) {
    List<Dispatcher.Frame> frames = new java.util.ArrayList<>();
    for (PolyglotException.StackFrame f : e.getPolyglotStackTrace()) {
      if (!f.isGuestFrame() || f.getSourceLocation() == null) continue;
      var loc = f.getSourceLocation();
      var src = loc.getSource();
      if (!isRoutineSource(src)) continue;
      String root = f.getRootName() == null ? "" : f.getRootName();
      if (root.equals(JsOracle.WRAPPER)) continue;  // ours, not the routine's
      int line = loc.getStartLine();
      frames.add(new Dispatcher.Frame(root, src.getName(), line, bodyColumn(src, line, loc.getStartColumn()),
          loc.getEndLine() == line ? bodyColumn(src, line, loc.getEndColumn()) : loc.getEndColumn()));
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
