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
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.time.Duration;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
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
  /** GraalPy is optional (package percona-mle-python, 21-packaging.md §2). */
  static final boolean PYTHON = installed("com.oracle.graal.python.PythonLanguage");
  /** GraalJS (always in the JVM worker's jar; looked up as GraalPy is). */
  static final boolean JS = installed("com.oracle.truffle.js.lang.JavaScriptLanguage");
  static final List<String> LANGUAGES = JS && PYTHON ? List.of("js", "python")
      : JS ? List.of("js") : PYTHON ? List.of("python") : List.of();

  private static boolean installed(String languageClass) {
    try {
      Class.forName(languageClass, false, Polyglot.class.getClassLoader());
      return true;
    } catch (ClassNotFoundException | LinkageError e) {
      return false;
    }
  }

  /**
   * What JavaScript and Python may use of Java objects: everything (polyglot reach: DBA jars,
   * java.util, ...) except reflection, class loading, threads and the process (15 §1; the OS
   * lockdown stays the boundary, ThreadWatch the backstop for threads).
   */
  static final HostAccess HOST_ACCESS = HostAccess.newBuilder(HostAccess.ALL)
      .denyAccess(Class.class)
      .denyAccess(ClassLoader.class)
      .denyAccess(java.lang.reflect.AccessibleObject.class)
      .denyAccess(java.lang.invoke.MethodHandles.class)
      .denyAccess(java.lang.invoke.MethodHandles.Lookup.class)
      .denyAccess(Thread.class)
      .denyAccess(ThreadGroup.class)
      .denyAccess(Runtime.class)
      .denyAccess(System.class)
      .denyAccess(ProcessBuilder.class)
      .denyAccess(ProcessHandle.class)
      // Mle.context() is for Java routines; closing it from a script would end the worker's use.
      .denyAccess(Context.class)
      .denyAccess(java.util.concurrent.Executors.class)
      .denyAccess(java.util.concurrent.ThreadPoolExecutor.class)
      .denyAccess(java.util.concurrent.ForkJoinPool.class)
      .denyAccess(java.util.concurrent.CompletableFuture.class)
      .denyAccess(java.util.Timer.class)
      // JMX (exp note 30): MXBeans reach JVM options, heap dumps, GC and thread control.
      .denyAccess(java.lang.management.ManagementFactory.class)
      .denyAccess(java.lang.management.PlatformManagedObject.class)
      .denyAccess(javax.management.MBeanServer.class)
      .build();

  /** Phase 7: guest code cannot start threads through Java either. */
  private static final java.util.Set<String> THREAD_CLASSES = java.util.Set.of("java.lang.Thread",
      "java.lang.ThreadGroup", "java.lang.Thread$Builder", "java.util.concurrent.Executors",
      "java.util.concurrent.ThreadPoolExecutor", "java.util.concurrent.ScheduledThreadPoolExecutor",
      "java.util.concurrent.ForkJoinPool", "java.util.concurrent.CompletableFuture", "java.util.Timer");

  /** JMX (exp note 30): no class of the management packages can be looked up. */
  private static final String[] MANAGEMENT_PACKAGES = {"java.lang.management.", "com.sun.management.",
      "javax.management.", "jdk.management.", "sun.management."};

  /** Whether guest code (Java.type, java.type) may look up the class. */
  static boolean lookupAllowed(String name) {
    if (name.startsWith("com.percona.mle.worker.") || THREAD_CLASSES.contains(name)) return false;
    for (String p : MANAGEMENT_PACKAGES)
      if (name.startsWith(p)) return false;
    return true;
  }

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

  /**
   * OPTIONS: the JavaScript converters read the integer and decimal types from $o; JavaScript's
   * local time is that of the zone of OPTIONS (Appendix B.4), and Python's TIMESTAMPs are aware
   * datetimes in it.
   */
  void applyOptions() {
    if (jsOracle != null) {
      Value o = jsOracle.getMember("$o");
      o.putMember("int", options.integerType);
      o.putMember("dec", options.decimalType);
      jsZone();
    }
    if (pySql != null) pySql.zone(options.zoneName);
  }

  /** The zone of JavaScript's local time now. */
  private String jsZone;

  /**
   * The ICU id the V8 worker gives the zone of the options (Date.prototype.toString() shows its
   * name): +00:00 is UTC, a whole hour Etc/GMT with IANA's sign (west positive), another offset
   * GMT+05:30; an IANA name as it is. java.time knows them all.
   */
  private String jsZoneName() {
    String name = options.zoneName;
    if (Options.isOffset(name) && name.length() == 6) {
      int hours = Integer.parseInt(name.substring(1, 3));
      if (name.substring(1).equals("00:00")) return "UTC";
      // (IANA has Etc/GMT-14 to Etc/GMT+12: -13:00 is GMT-13:00.)
      if (name.endsWith(":00") && (name.charAt(0) == '+' || hours <= 12))
        return "Etc/GMT" + (name.charAt(0) == '+' ? "-" : "+") + hours;
      return "GMT" + name;
    }
    if (!Options.isOffset(name))
      return options.zone instanceof java.time.ZoneOffset ? "UTC" : options.zone.getId();  // "UTC", "Z"
    return name;
  }

  /**
   * GraalJS takes the zone of local time from the context (the process's zone, or the builder's
   * timeZone()), fixed when the context is made; the context lives as long as the worker and
   * serves sessions of any zone. So the JavaScript realm's zone is set directly (JSRealm is in this
   * class loader, as the FrameLimit instrument is), also when the context is made, so that a
   * worker of the pool has done it once; the main thread is entered. Options change only while
   * nothing is prepared (§9.8).
   */
  private void jsZone() {
    String name = jsZoneName();
    if (name.equals(jsZone)) return;
    try {
      if (getMainRealm == null) {
        // Method handles and var handles: a reflective Field.set makes an accessor per field on
        // its first use (~1.3 ms for these, at every worker's start).
        Class<?> realm = Class.forName("com.oracle.truffle.js.runtime.JSRealm", true, Polyglot.class.getClassLoader());
        MethodHandles.Lookup lookup = MethodHandles.privateLookupIn(realm, MethodHandles.lookup());
        getMainRealm = lookup.findStatic(realm, "getMain", MethodType.methodType(realm, com.oracle.truffle.api.nodes.Node.class))
            .asType(MethodType.methodType(Object.class, com.oracle.truffle.api.nodes.Node.class));
        try {
          java.lang.invoke.VarHandle[] f = new java.lang.invoke.VarHandle[REALM_ZONE_FIELDS.length];
          for (int i = 0; i < f.length; i++) f[i] = lookup.unreflectVarHandle(realm.getDeclaredField(REALM_ZONE_FIELDS[i]));
          realmZoneFields = f;
        } catch (ReflectiveOperationException | RuntimeException e) {
          setLocalTimeZone = realm.getMethod("setLocalTimeZone", String.class);  // another GraalJS
        }
      }
      Object realm = (Object) getMainRealm.invokeExact((com.oracle.truffle.api.nodes.Node) null);
      if (realmZoneFields != null) {
        // What JSRealm.setLocalTimeZone does, but for its ICU TimeZone: that one is made when
        // Intl first needs it, as for a new realm (making it loads ICU's zone data, ~10 ms at
        // the worker's start).
        realmZoneFields[0].set(realm, (Object) java.time.ZoneId.of(name));
        for (int i = 1; i < realmZoneFields.length; i++) realmZoneFields[i].set(realm, (Object) null);
      } else {
        setLocalTimeZone.invoke(realm, name);
      }
      // The Date methods keep a name only when they asked for one: else they have none to forget.
      boolean named = zoneNames.takeAsked();
      zoneNames.zone(name);
      if (jsZoneChanged != null && named) jsZoneChanged.executeVoid();
    } catch (RuntimeException | Error e) {
      throw e;
    } catch (Throwable e) {
      // GraalJS without this method: local time stays the process's (a build problem, not the session's)
      throw new IllegalStateException("cannot set JavaScript's time zone: " + e, e);
    }
    jsZone = name;
  }

  private static MethodHandle getMainRealm;
  private static java.lang.reflect.Method setLocalTimeZone;
  /** JSRealm's zone (java.time), then what it keeps made for the zone: its ICU TimeZone, Date formats. */
  private static final String[] REALM_ZONE_FIELDS = {"localTimeZoneId", "localTimeZone", "jsDateToStringFormat",
      "jsShortTimeFormat", "jsShortTimeLocalFormat", "jsShortDateFormat", "jsShortDateLocalFormat"};
  private static java.lang.invoke.VarHandle[] realmZoneFields;

  /** Tells the Date methods below that the zone changed (they keep its name for a span of time). */
  private Value jsZoneChanged;

  private final ZoneNames zoneNames = new ZoneNames();

  /**
   * The name V8 gives JavaScript's local time zone in Date.prototype.toString(): ICU's long name
   * of the zone for standard or for daylight saving time, as the zone names them today
   * (icu::TimeZone::getDisplayName(isDst, LONG), which V8 keeps per zone: "Eastern European
   * Summer Time", "Coordinated Universal Time", "GMT+05:30" for a zone without a name), by whether
   * ICU's zone has daylight saving time then. name(t) also gives the span [from, until) around t
   * (ms) between two of the zone's transitions, in which the name stays: the Date methods keep
   * the name and ask again only outside the span. GraalJS's ICU; main thread only.
   */
  public static final class ZoneNames {
    /** The span of the last name(t), in ms since the epoch; empty before the first. */
    public double from = 1, until = 0;
    private String id;
    private Object zone;  // ICU's TimeZone, made on first use, as the names
    private String standard, daylight;
    private final int[] offsets = new int[2];

    /** The zone of local time now: its ICU id. */
    void zone(String icuId) {
      if (icuId.equals(id)) return;
      id = icuId;
      zone = null;
      standard = daylight = null;
      from = 1;
      until = 0;
    }

    /** Whether name() was called since the last takeAsked(): the Date methods may keep a name. */
    boolean takeAsked() {
      boolean a = asked;
      asked = false;
      return a;
    }

    private boolean asked;

    public String name(double t) {
      asked = true;
      try {
        if (zone == null) zone = (Object) Icu.GET_TIME_ZONE.invokeExact(id);
        long ms = (long) t;
        // V8 asks whether a time before 1970 or after 2038 has daylight saving time in an
        // equivalent year (2008-2035: as leap, beginning on the same weekday): at the time
        // shifted by delta, which holds in [lo, hi) (t's UTC year, on t's side of that range).
        long delta = 0, lo = 0, hi = MAX_EPOCH_MS + 1;
        if (ms < 0 || ms > MAX_EPOCH_MS) {
          int year = java.time.LocalDate.ofEpochDay(Math.floorDiv(ms, DAY_MS)).getYear();
          lo = java.time.LocalDate.of(year, 1, 1).toEpochDay() * DAY_MS;
          hi = java.time.LocalDate.of(year + 1, 1, 1).toEpochDay() * DAY_MS;
          delta = java.time.LocalDate.of(equivalentYear(year), 1, 1).toEpochDay() * DAY_MS - lo;
          if (ms < 0) hi = Math.min(hi, 0);
          else lo = Math.max(lo, MAX_EPOCH_MS + 1);
        }
        long at = ms + delta;
        Icu.GET_OFFSET.invokeExact(zone, at, false, offsets);
        // The transitions around: at or before the time, and the first one after it.
        Object prev = (Object) Icu.PREVIOUS_TRANSITION.invokeExact(zone, at, true);
        Object next = (Object) Icu.NEXT_TRANSITION.invokeExact(zone, at, false);
        from = Math.max(lo, prev == null ? Long.MIN_VALUE : (long) Icu.TRANSITION_TIME.invokeExact(prev) - delta);
        until = Math.min(hi, next == null ? Long.MAX_VALUE : (long) Icu.TRANSITION_TIME.invokeExact(next) - delta);
        boolean dst = offsets[1] != 0;
        String name = dst ? daylight : standard;
        if (name == null) {
          name = (String) Icu.DISPLAY_NAME.invokeExact(zone, dst, Icu.LONG, Icu.US);
          if (dst) daylight = name;
          else standard = name;
        }
        return name;
      } catch (Throwable e) {
        throw new IllegalStateException("cannot name JavaScript's time zone: " + e, e);
      }
    }

    /**
     * GraalJS's ICU (com.ibm.icu shadowed; only GraalJS has it on its class path): TimeZone,
     * BasicTimeZone, TimeZoneTransition and ULocale, loaded on first use.
     */
    private static final class Icu {
      static final MethodHandle GET_TIME_ZONE, GET_OFFSET, PREVIOUS_TRANSITION, NEXT_TRANSITION,
          TRANSITION_TIME, DISPLAY_NAME;
      static final int LONG;
      static final Object US;

      static {
        try {
          String p = "org.graalvm.shadowed.com.ibm.icu.util.";
          ClassLoader l = Polyglot.class.getClassLoader();
          Class<?> tz = Class.forName(p + "TimeZone", true, l);
          Class<?> basic = Class.forName(p + "BasicTimeZone", true, l);
          Class<?> transition = Class.forName(p + "TimeZoneTransition", true, l);
          Class<?> locale = Class.forName(p + "ULocale", true, l);
          var lookup = MethodHandles.publicLookup();
          GET_TIME_ZONE = lookup.findStatic(tz, "getTimeZone", MethodType.methodType(tz, String.class))
              .asType(MethodType.methodType(Object.class, String.class));
          GET_OFFSET = lookup.findVirtual(tz, "getOffset",
              MethodType.methodType(void.class, long.class, boolean.class, int[].class))
              .asType(MethodType.methodType(void.class, Object.class, long.class, boolean.class, int[].class));
          PREVIOUS_TRANSITION = lookup.findVirtual(basic, "getPreviousTransition",
              MethodType.methodType(transition, long.class, boolean.class))
              .asType(MethodType.methodType(Object.class, Object.class, long.class, boolean.class));
          NEXT_TRANSITION = lookup.findVirtual(basic, "getNextTransition",
              MethodType.methodType(transition, long.class, boolean.class))
              .asType(MethodType.methodType(Object.class, Object.class, long.class, boolean.class));
          TRANSITION_TIME = lookup.findVirtual(transition, "getTime", MethodType.methodType(long.class))
              .asType(MethodType.methodType(long.class, Object.class));
          DISPLAY_NAME = lookup.findVirtual(tz, "getDisplayName",
              MethodType.methodType(String.class, boolean.class, int.class, locale))
              .asType(MethodType.methodType(String.class, Object.class, boolean.class, int.class, Object.class));
          LONG = tz.getField("LONG").getInt(null);
          US = locale.getField("US").get(null);
        } catch (ReflectiveOperationException e) {
          throw new ExceptionInInitializerError(e);
        }
      }
    }

    private static final long DAY_MS = 86_400_000L, MAX_EPOCH_MS = Integer.MAX_VALUE * 1000L;

    /** V8's DateCache::EquivalentYear (ECMA-262 5.1, 15.9.1.8). */
    private static int equivalentYear(int year) {
      int weekDay = (int) Math.floorMod(java.time.LocalDate.of(year, 1, 1).toEpochDay() + 4, 7L);
      int recent = (java.time.Year.isLeap(year) ? 1956 : 1967) + (weekDay * 12) % 28;
      return 2008 + (recent + 3 * 28 - 2008) % 28;
    }
  }

  /**
   * Date.prototype.toString() and toTimeString() as V8 (and Oracle's MLE) show them: GraalJS names
   * the zone in short ("GMT+3", "EET") unless in its V8 compatibility mode, which changes much else;
   * V8 gives ICU's long name (ZoneNames). The rest is GraalJS's own (its date, time and offset are
   * V8's): its text with the long name in place of the short one.
   */
  static final String DATE_STRINGS = """
      ((zones) => {
        const P = Date.prototype, toString = P.toString, toTimeString = P.toTimeString, getTime = P.getTime;
        let from = Infinity, until = -Infinity, name = '';  // numbers as ZoneNames gives them (doubles)
        const named = (s, t) => {
          if (!(t >= from && t < until)) {
            name = zones.name(t);
            from = zones.from;
            until = zones.until;
          }
          return s.slice(0, s.lastIndexOf(' (') + 2) + name + ')';
        };
        const methods = {
          toString() {
            const t = getTime.call(this), s = toString.call(this);
            return t === t ? named(s, t) : s;
          },
          toTimeString() {
            const t = getTime.call(this), s = toTimeString.call(this);
            return t === t ? named(s, t) : s;
          },
        };
        Object.defineProperty(P, 'toString', { value: methods.toString, writable: true, enumerable: false, configurable: true });
        Object.defineProperty(P, 'toTimeString', { value: methods.toTimeString, writable: true, enumerable: false, configurable: true });
        return () => { from = Infinity; until = -Infinity; };
      })
      """;

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
      long until = System.nanoTime() + 10_000_000_000L;
      while (interrupter.isAlive() && until - System.nanoTime() > 0) {
        try {
          interrupter.join(Math.max(1, (until - System.nanoTime()) / 1_000_000));
        } catch (InterruptedException e) {
          // the call has ended (Control.end): an interrupt now belongs to no call
        }
      }
      Thread.interrupted();
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
      if (JS) c.getBindings("js").putMember("Mle", mleApi);
      c.getPolyglotBindings().putMember("Mle", mleApi);
      if (JS) {
        js = jsHelpers(c);
        jsOracle = JsOracle.helpers(c);
        jsZoneChanged = jsOracle.getMember("$mle$dates").execute(zoneNames);
        // USING in JavaScript routines (Phase 6): $c.lib(...) gets every library's module.
        jsOracle.putMember("libBridge",
            (org.graalvm.polyglot.proxy.ProxyExecutable) a -> libraryModule(a[0].asString(), a[1].asString(), true));
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
    Context.Builder b = Context.newBuilder(LANGUAGES.toArray(new String[0]))
        .allowHostAccess(HOST_ACCESS)
        // The process is the sandbox; only the worker's own classes are hidden.
        .allowHostClassLookup(Polyglot::lookupAllowed)
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
        .allowExperimentalOptions(true)
        .option("engine.WarnInterpreterOnly", "false")
        .option(FrameLimit.ID, "true")
        .environment("HOME", "/nonexistent");  // GraalPy looks up the home directory (U-1)
    // Evaluating a module returns its exports: database libraries and ES modules are loaded
    // synchronously (an import() promise cannot settle while a JavaScript frame is on the
    // stack, as in nested calls). GraalJS options exist only when GraalJS is there (23).
    if (JS) b.option("js.esm-eval-returns-exports", "true");
    // The DBA's jars (Phase 6) for Java.type() in JavaScript and java.type() in Python (JVM only).
    b.hostClassLoader(JavaRoutines.hostLoader());
    // Python is an optional package: its options exist only when GraalPy is installed.
    if (PYTHON)
      b.option("python.PosixModuleBackend", "java")
          .option("python.DontWriteBytecodeFlag", "true")
          .option("python.NoUserSiteFlag", "true")
          .option("python.WarnExperimentalFeatures", "false");
    // Third-party packages installed by the DBA (14 §3): npm packages for require() and
    // import of bare names (CommonJS support is an experimental GraalJS option), Python
    // packages on the module path.
    List<String> pythonPath = new java.util.ArrayList<>();
    String packages = System.getProperty("mle.packages", "");
    if (!packages.isEmpty()) {
      Path js = Path.of(packages, "js");
      if (JS && java.nio.file.Files.isDirectory(js.resolve("node_modules")))
        b.allowExperimentalOptions(true)
            .option("js.commonjs-require", "true")
            .option("js.commonjs-require-cwd", js.toString())
            .option("js.esm-bare-specifier-relative-lookup", "true");
      Path site = Path.of(packages, "python", "site-packages");
      if (java.nio.file.Files.isDirectory(site)) pythonPath.add(site.toString());
    }
    if (PYTHON && !pythonPath.isEmpty()) b.option("python.PythonPath", String.join(java.io.File.pathSeparator, pythonPath));
    // The build's venv (numpy): GraalPy's site finds it from sys.executable's directory and its
    // pyvenv.cfg. Only the path is used: the installed bin/python is a placeholder (the venv's own
    // points to the GraalPy that built it, which is not installed with MLE).
    String venv = System.getProperty("mle.python.venv", "");
    if (PYTHON && !venv.isEmpty())
      b.option("python.Executable", Path.of(venv).resolve("bin/python").toString())
          .option("python.ForceImportSite", "true");
    if (console != null) {
      b.out(console.outStream()).err(console.errStream());
      // The engine's and the languages' own log lines (GraalPy's C API notices, Truffle) are the
      // worker's diagnostics, not the routine's output (exp note 39): the worker's stderr.
      b.logHandler(console.log());
    }
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
        # threading keeps its own references to the _thread functions.
        for _mle_name in ('_start_joinable_thread', '_start_new_thread', '_start_thread'):
            if hasattr(_mle_threading, _mle_name):
                setattr(_mle_threading, _mle_name, _mle_no_threads)

        # Phase 7 (S-14): numpy arrays the host fills with one memcpy; creating numpy arrays is
        # the expensive part, so argument arrays are reused (read-only, valid during the call).
        def _numpy_buffer(shape, readonly):
            try:
                import numpy
            except ImportError:
                return None
            a = numpy.empty(shape, dtype=numpy.float32)
            ptr = a.__array_interface__['data'][0]
            if readonly:
                # The routine gets a view: the host writes the next call's vector to ptr, so the
                # array that owns that memory must not be resized (a view cannot be).
                v = a.view()
                v.setflags(write=False)
                return (v, ptr)
            return (a, ptr)

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

        # Errors of JavaScript and Java in Python code (exp note 41). GraalPy gives Python the
        # other languages' exceptions as polyglot.ForeignException, which derives from
        # BaseException only: the usual `except Exception` let them through. Java's become
        # Exceptions for all Python code (register_interop_type); for JavaScript's, the code MLE
        # compiles (routine bodies, libraries, mle_eval) gets ForeignException added where it
        # says `except Exception`. (A single underscore: a dunder name is mangled in classes.)
        import ast as _mle_ast, polyglot as _mle_polyglot
        _mle_globals['_mle_ForeignException'] = _mle_polyglot.ForeignException
        try:
            import java as _mle_java

            class JavaError(Exception):
                pass  # Java exceptions in Python code (they are also polyglot.ForeignException)

            _mle_polyglot.register_interop_type(_mle_java.type('java.lang.Throwable'), JavaError)
        except BaseException:
            pass  # no Java interop: Java exceptions stay polyglot.ForeignException

        def _mle_register_row(row_class):
            # Python's rows (exp note 41): row['label'] besides row.label and row[0]; iteration and
            # len() stay a tuple's.
            class Row:
                def __getitem__(self, key):
                    if isinstance(key, str):
                        try:
                            return getattr(self, key)
                        except AttributeError:
                            raise KeyError(key) from None
                    return _mle_polyglot.ForeignList.__getitem__(self, key)
            _mle_polyglot.register_interop_type(row_class, Row, allow_method_overwrites=True)

        class _MleCatchForeign(_mle_ast.NodeTransformer):
            def visit_ExceptHandler(self, node):
                self.generic_visit(node)
                def is_exception(n):
                    return isinstance(n, _mle_ast.Name) and n.id == 'Exception'
                extra = _mle_ast.Name(id='_mle_ForeignException', ctx=_mle_ast.Load())
                if is_exception(node.type):
                    node.type = _mle_ast.Tuple(elts=[node.type, extra], ctx=_mle_ast.Load())
                elif isinstance(node.type, _mle_ast.Tuple) and any(is_exception(e) for e in node.type.elts):
                    node.type.elts.append(extra)
                return node

        def _mle_compile(tree, name):
            # A tree with `except Exception` catching foreign errors, compiled. A SyntaxError found
            # after parsing (import * in a function, a misplaced nonlocal) is raised again with its
            # location: GraalPy fails (UnsupportedOperationException) to pass one of a compiled
            # tree to Java.
            tree = _mle_ast.fix_missing_locations(_MleCatchForeign().visit(tree))
            try:
                return compile(tree, name, 'exec')
            except SyntaxError as e:
                raise SyntaxError(e.msg, (name, e.lineno, e.offset, e.text, e.end_lineno, e.end_offset)) from None

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
            try:
                tree = ast.parse(textwrap.dedent(src), filename=name)
            except (IndentationError, SyntaxError):
                # The first statement on the $$ line, the rest indented below it: dedent the
                # lines after the first one on their own.
                first, _, rest = src.partition('\\n')
                if not first.strip() or not rest.strip():
                    raise
                tree = ast.parse(first.strip() + '\\n' + textwrap.dedent(rest), filename=name)
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
            code = _mle_compile(mod, name)
            ns = {'__name__': name, 'Mle': mle}
            ns.update(_mle_globals)
            exec(code, ns)
            # Phase 7: fresh globals per call; state belongs in libraries (15 §1). Each call runs
            # the routine's code with a copy of the prepared globals.
            code, base, make = ns['__mle_routine'].__code__, ns, _mle_types_mod.FunctionType
            def fresh(*args):
                return make(code, base.copy())(*args)
            fresh.__mle_base__ = base
            return fresh

        def _check_names(src, params, known):
            # CREATE (exp note 39): names the code reads but never binds (a typo such as lenght),
            # not built-ins, parameters, MLE's globals or USING aliases. Scopes are not told
            # apart, so it never reports a name bound anywhere; exec/eval/globals/vars and star
            # imports turn it off. Line numbers are the body's.
            import ast, textwrap, builtins
            try:
                tree = ast.parse(textwrap.dedent(src))
            except SyntaxError:
                first, _, rest = src.partition('\\n')
                try:
                    tree = ast.parse(first.strip() + '\\n' + textwrap.dedent(rest))
                except SyntaxError:
                    return []
            bound = set(params) | set(known) | set(dir(builtins)) | set(_mle_globals)
            bound |= {'Mle', '__name__', '__file__', '__doc__'}
            for n in ast.walk(tree):
                if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
                    bound.add(n.id)
                elif isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                    bound.add(n.name)
                elif isinstance(n, ast.arg):
                    bound.add(n.arg)
                elif isinstance(n, (ast.Import, ast.ImportFrom)):
                    for a in n.names:
                        if a.name == '*':
                            return []
                        bound.add((a.asname or a.name).split('.')[0])
                elif isinstance(n, ast.ExceptHandler) and n.name:
                    bound.add(n.name)
                elif isinstance(n, (ast.Global, ast.Nonlocal)):
                    bound.update(n.names)
                elif type(n).__name__ in ('MatchAs', 'MatchStar') and getattr(n, 'name', None):
                    bound.add(n.name)
                elif type(n).__name__ == 'MatchMapping' and getattr(n, 'rest', None):
                    bound.add(n.rest)
                elif (isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
                      and n.func.id in ('exec', 'eval', 'globals', 'locals', 'vars')):
                    return []
            found = {}
            for n in ast.walk(tree):
                if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load) and n.id not in bound:
                    found.setdefault(n.id, n.lineno)
            return ["line %d: name '%s' is not defined" % (line, name)
                    for name, line in sorted(found.items(), key=lambda x: x[1])]

        def _load_eval(src, mle):
            # mle_eval (exp note 39): the snippet's statements, then the value of its last
            # expression as text (repr, as a REPL shows it; a str as it is), or None.
            import ast, textwrap
            tree = ast.parse(textwrap.dedent(src), filename='mle_eval')
            tail = None
            if tree.body and isinstance(tree.body[-1], ast.Expr):
                tail = compile(ast.Expression(tree.body.pop().value), 'mle_eval', 'eval')
            body = _mle_compile(tree, 'mle_eval')
            def run():
                ns = {'__name__': 'mle_eval', 'Mle': mle}
                ns.update(_mle_globals)
                exec(body, ns)
                if tail is None:
                    return None
                v = eval(tail, ns)
                return None if v is None else v if isinstance(v, str) else repr(v)
            return run

        # numpy from JavaScript and Java (exp note 41). GraalPy gives Python their arrays as
        # polyglot.ForeignList (a list subclass) that numpy's C code cannot read (RecursionError),
        # and numpy's scalars reach the other languages as objects. So: (A) at numpy's first
        # import its scalar types become numbers for the other languages; (B2) numpy.array,
        # asarray and asanyarray take foreign arrays; (B1) modules imported from JavaScript or
        # Java convert the arguments of their functions. Arrays are copied, as VECTOR arguments.
        import importlib as _mle_importlib, importlib.util as _mle_importlib_util
        import polyglot as _mle_polyglot, sys as _mle_sys

        def _numpy_empty(shape, dtype):
            # For the host's bulk copy (Polyglot.bulkArray): a new array and its data address.
            import numpy
            a = numpy.empty(shape, dtype=dtype)
            return (a, a.__array_interface__['data'][0])

        def _mle_to_py(v, numpy_ok=True):
            # A foreign array as a numpy array (numpy_ok) or a Python list; nested ones too.
            if isinstance(v, _mle_polyglot.ForeignList):
                bulk = _mle_helpers.get('bulk')
                a = bulk(v) if bulk is not None else None
                if a is not None:
                    return a if numpy_ok else a.tolist()
                return [_mle_to_py(x, numpy_ok) for x in v]
            if type(v) in (list, tuple) and v and isinstance(v[0], _mle_polyglot.ForeignList):
                return type(v)(_mle_to_py(x, numpy_ok) for x in v)
            return v

        def _mle_numpy_ready(np):
            def integral(v):
                x = v.item()
                if isinstance(x, int):
                    return int(x)
                if isinstance(x, float) and x.is_integer():
                    return int(x)
                return None
            def fits(lo, hi):
                def f(v):
                    i = integral(v)
                    return i is not None and lo <= i <= hi
                return f
            def fits_float(v):
                x = float(v.item())
                return x != x or x in (float('inf'), float('-inf')) or float(np.float32(x)) == x
            def fits_double(v):
                x = v.item()
                return isinstance(x, float) or -2 ** 53 <= x <= 2 ** 53
            number = dict(
                is_number=True,
                fits_in_byte=fits(-2 ** 7, 2 ** 7 - 1), fits_in_short=fits(-2 ** 15, 2 ** 15 - 1),
                fits_in_int=fits(-2 ** 31, 2 ** 31 - 1), fits_in_long=fits(-2 ** 63, 2 ** 63 - 1),
                fits_in_big_integer=lambda v: integral(v) is not None,
                fits_in_float=fits_float, fits_in_double=fits_double,
                as_byte=integral, as_short=integral, as_int=integral, as_long=integral,
                as_big_integer=integral,
                as_float=lambda v: float(v.item()), as_double=lambda v: float(v.item()))
            for t in (np.int8, np.int16, np.int32, np.int64, np.uint8, np.uint16, np.uint32,
                      np.uint64, np.float16, np.float32, np.float64):
                _mle_polyglot.register_interop_behavior(t, **number)
            _mle_polyglot.register_interop_behavior(np.bool_, is_boolean=True,
                                                    as_boolean=lambda v: bool(v))
            for name in ('array', 'asarray', 'asanyarray'):
                def taking_foreign(obj, *args, _orig=getattr(np, name), **kwargs):
                    return _orig(_mle_to_py(obj), *args, **kwargs)
                orig = getattr(np, name)
                taking_foreign.__name__ = orig.__name__
                taking_foreign.__qualname__ = orig.__name__
                taking_foreign.__doc__ = orig.__doc__
                taking_foreign.__wrapped__ = orig
                setattr(np, name, taking_foreign)

        class _MleNumpyHook:
            # Runs _mle_numpy_ready once, right after numpy is first imported (by any route).
            busy = False
            def find_spec(self, name, path=None, target=None):
                if name != 'numpy' or self.busy:
                    return None
                self.busy = True
                try:
                    spec = _mle_importlib_util.find_spec('numpy')
                finally:
                    self.busy = False
                if spec is None or spec.loader is None:
                    return spec
                hook, run = self, spec.loader.exec_module
                def exec_module(module):
                    run(module)
                    _mle_numpy_ready(module)
                    if hook in _mle_sys.meta_path:
                        _mle_sys.meta_path.remove(hook)
                spec.loader.exec_module = exec_module
                return spec

        if 'numpy' in _mle_sys.modules:
            _mle_numpy_ready(_mle_sys.modules['numpy'])
        else:
            _mle_sys.meta_path.insert(0, _MleNumpyHook())

        class _MleCallable:
            # A function of a module imported from JavaScript or Java: foreign array arguments
            # are converted first (numpy arrays for numpy, Python lists for other modules).
            __slots__ = ('_f', '_np')
            def __init__(self, f, np):
                object.__setattr__(self, '_f', f)
                object.__setattr__(self, '_np', np)
            def __call__(self, *args, **kwargs):
                np = self._np
                return self._f(*[_mle_to_py(a, np) for a in args],
                               **{k: _mle_to_py(v, np) for k, v in kwargs.items()})
            def __getattr__(self, name):
                return getattr(self._f, name)
            def __repr__(self):
                return repr(self._f)

        class _MleModule:
            __slots__ = ('_m', '_np')
            def __init__(self, m):
                object.__setattr__(self, '_m', m)
                object.__setattr__(self, '_np', m.__name__.split('.')[0] == 'numpy')
            def __getattr__(self, name):
                v = getattr(self._m, name)
                if isinstance(v, _mle_types_mod.ModuleType):
                    return _MleModule(v)
                if callable(v):
                    return _MleCallable(v, self._np)
                return v
            def __dir__(self):
                return dir(self._m)
            def __repr__(self):
                return repr(self._m)

        def _import_for_foreign(name):
            # importPython (JavaScript) and Mle.importPython (Java).
            return _MleModule(_mle_importlib.import_module(name))

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
            'body_loader': _load_body, 'eval_loader': _load_eval, 'check_names': _check_names,
            'numpy_buffer': _numpy_buffer,
            'numpy_numbers': _numpy_numbers,
            'numpy_matrix': _numpy_matrix,
            'numpy_empty': _numpy_empty,
            'import_foreign': _import_for_foreign,
            'register_row': _mle_register_row,
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

  /** Set by the dispatcher for a CREATE's PREPARE: also check the code (Python names). */
  boolean check;

  /** What the check of the PREPARE being served found (RESULT's findings, §9.4). */
  private List<String> findings = List.of();

  /** The findings of the last PREPARE with check; none are left behind. */
  List<String> takeFindings() {
    List<String> f = findings;
    findings = List.of();
    return f;
  }

  /** CREATE (exp note 39): the check's findings, one per name (warnings in mysqld). */
  private void checkPython(String source, List<JavaRoutines.Param> params, List<LibraryFileSystem.Import> imports) {
    try {
      List<String> names = new java.util.ArrayList<>();
      for (JavaRoutines.Param p : params) names.add(p.name());
      List<String> known = new java.util.ArrayList<>();
      for (LibraryFileSystem.Import i : imports) known.add(i.alias().isEmpty() ? i.name() : i.alias());
      Value found = context.getBindings("python").getMember("_mle_helpers").getHashValue("check_names")
          .execute(source, names.toArray(), known.toArray());
      List<String> list = new java.util.ArrayList<>();
      for (long i = 0; i < found.getArraySize() && list.size() < Wire.MAX_FINDINGS; i++)
        list.add(found.getArrayElement(i).asString());
      findings = list;
    } catch (PolyglotException e) {
      // A check that fails is no reason to refuse the routine; KILL still stops the statement.
      if (e.isInterrupted() || e.isCancelled() || e.isResourceExhausted()) throw e;
    } catch (RuntimeException e) {
      // The same for a host-side failure (an unexpected result type, a GraalPy bug).
    }
  }

  /** JavaScript values of mle_eval as text (JSON for objects and arrays, else String(v)). */
  private Value jsShow;

  /**
   * mle_eval (exp note 39): a snippet as a routine without parameters whose result is the value
   * of its last expression as text. JavaScript runs it as a strict-mode block (let/const and
   * functions stay in it; the script's completion value is the block's), with the routine's URI so
   * that errors have their location; Python through _load_eval (fresh globals per call).
   */
  void prepareEval(long id, String language, String code, Values.ParamType ret)
      throws JavaRoutines.CompileException {
    Context c = context();
    Value fn;
    try {
      if (language.equals("js")) {
        java.net.URI uri = routineUri(id);
        String prefix = "'use strict';{";
        prefixes.put(uri, prefix.length());
        // Syntax errors now, as for a routine, and of the code as written (not of the block).
        c.parse(Source.newBuilder("js", code, "mle_eval").buildLiteral());
        Source src = Source.newBuilder("js", prefix + code + "\n}", "mle_eval").uri(uri).buildLiteral();
        if (jsShow == null)
          jsShow = c.eval("js", "(v) => v === undefined || v === null ? null : typeof v === 'string' ? v"
              + " : typeof v === 'object' ? (() => { try { return JSON.stringify(v) } catch (e) { return String(v) } })()"
              + " : String(v)");
        Value show = jsShow;
        fn = c.asValue((org.graalvm.polyglot.proxy.ProxyExecutable) a -> show.execute(c.eval(src)));
      } else {
        pythonReady();
        pythonSources.add("mle_eval");
        SOURCE_LINES.put("mle_eval", code.split("\n", -1));
        fn = context.getBindings("python").getMember("_mle_helpers").getHashValue("eval_loader")
            .execute(code, mleApi);
      }
    } catch (PolyglotException e) {
      if (e.isInterrupted() || e.isCancelled() || e.isResourceExhausted()) throw e;
      // The message alone: JavaScript's names the place itself, and the server adds the location
      // of the frames (Python).
      throw new JavaRoutines.CompileException(describe(e, false));
    }
    routines.put(id, new Routine(language, fn, new Value[0], ret, new Values.ParamType[0], new int[0], null,
        List.of()));
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
        return jsLibrary(schema, name, l.stamp());
      }
      case "python" -> {
        pythonReady();
        // PySql keeps evaluated ones by stamp: this says whether it evaluates now.
        Long seen = pyLibraries.get(schema + "/" + name);
        if (seen == null || seen != l.stamp()) LibraryFileSystem.evaluating(schema, name);
        Object module = pySql.loadLibrary.execute(schema + "." + name, l.stamp(),
            new String(l.body(), java.nio.charset.StandardCharsets.UTF_8));
        if (seen == null || seen != l.stamp()) pyLibraries.put(schema + "/" + name, l.stamp());
        return module;
      }
      case "java" -> {
        // A Java library: compiled once per version into its own class loader; the namespace's
        // members are its classes (static side: static methods, constructors with new).
        return javaLibrary(schema + "." + name, l);
      }
      default -> throw new IllegalArgumentException("the library " + schema + "." + name + " is in the language "
          + l.language() + ", which this worker cannot run");
    }
  }

  /** Python libraries evaluated (in PySql's cache), by "schema/name": their stamps. */
  private final Map<String, Long> pyLibraries = new HashMap<>();

  /** JavaScript libraries' module namespaces, by "schema/name": {stamp, namespace}. */
  private final Map<String, Object[]> jsLibraries = new HashMap<>();

  private Value jsLibrary(String schema, String name, long stamp) {
    String key = schema + "/" + name;
    Object[] hit = jsLibraries.get(key);
    if (hit != null && (long) hit[0] == stamp) return (Value) hit[1];
    // The virtual path, not Oracle's "/schema/name": a host directory of that name does not hide
    // the library. The module map keeps one instance per version for every importer.
    LibraryFileSystem.checkJsName(schema, name);
    LibraryFileSystem.evaluating(schema, name);
    Value ns = jsModule(LibraryFileSystem.ROOT + "/" + schema + "/" + name + ".mjs");
    jsLibraries.put(key, new Object[] {stamp, ns});
    return ns;
  }

  private long moduleWrappers;

  /** An ES module's namespace, loaded and evaluated synchronously (no promise jobs). */
  private Value jsModule(String specifier) {
    Source s = Source.newBuilder("js", "import * as ns from " + JsOracle.jsString(specifier) + ";export default ns;",
            "mle-import-" + (++moduleWrappers) + ".mjs")
        .mimeType("application/javascript+module").buildLiteral();
    return context().eval(s).getMember("default");
  }

  private final Map<String, Object[]> javaLibraries = new HashMap<>();

  private Object javaLibrary(String key, LibraryFileSystem.Library l) {
    Object[] hit = javaLibraries.get(key);
    if (hit != null && (long) hit[0] == l.stamp()) return hit[1];
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
    javaLibraries.put(key, new Object[] {l.stamp(), ns});
    return ns;
  }

  /** importPython(name) / Mle.importPython: a Python module (numpy ...) for any language. */
  Value importPython(String module) {
    if (!PYTHON)
      throw new IllegalStateException("Python is not installed in this MLE (package percona-mle-python)");
    // Converts the arguments of the module's functions (exp note 41).
    return pythonHelper("import_foreign").execute(module);
  }

  /**
   * Exp note 41: a JavaScript or Java array of numbers as a new numpy array in one copy (numpy
   * cannot read foreign arrays): typed arrays and Java primitive arrays keep their element type,
   * JavaScript numbers and Java Lists of numbers become int64 (all integral) or float64, and
   * rectangular arrays of such rows become 2-D. Null for anything else (or without numpy): Python
   * then converts element by element.
   */
  @SuppressWarnings("restricted")  // the new array's own memory, as numpy reports it
  private Object bulkArray(Value v) {
    Object[] flat = null;  // {dtype, MemorySegment of the data, rows, columns (-1: 1-D)}
    if (v.isHostObject()) {
      flat = hostNumbers(v.asHostObject());
    } else if (JS && v.hasArrayElements()) {
      Value r = jsBulk().execute(v);
      if (!r.isNull()) {
        Value buf = r.getArrayElement(1);
        byte[] b = new byte[Math.toIntExact(buf.getBufferSize())];
        buf.readBuffer(0, b, 0, b.length);
        flat = new Object[] {r.getArrayElement(0).asString(), java.lang.foreign.MemorySegment.ofArray(b),
            r.getArrayElement(2).asInt(), r.getArrayElement(3).asInt()};
      }
    }
    if (flat == null) return null;
    int rows = (int) flat[2], cols = (int) flat[3];
    Value shape = cols < 0 ? context.asValue(rows) : context.eval("python", "lambda n, d: (n, d)").execute(rows, cols);
    Value t;
    try {
      t = pythonHelper("numpy_empty").execute(shape, flat[0]);
    } catch (org.graalvm.polyglot.PolyglotException e) {
      if (e.isGuestException() && String.valueOf(e.getMessage()).contains("No module named 'numpy'")) return null;
      throw e;
    }
    java.lang.foreign.MemorySegment data = (java.lang.foreign.MemorySegment) flat[1];
    // The new array holds exactly what is copied into it; anything else (a typed array subclass
    // whose length does not match its buffer) goes element by element.
    long item = switch ((String) flat[0]) {
      case "int8", "uint8" -> 1;
      case "int16", "uint16" -> 2;
      case "int32", "uint32", "float32" -> 4;
      default -> 8;
    };
    if (rows < 0 || (long) rows * (cols < 0 ? 1 : cols) * item != data.byteSize()) return null;
    java.lang.foreign.MemorySegment.ofAddress(t.getArrayElement(1).asLong()).reinterpret(data.byteSize())
        .copyFrom(data);
    return t.getArrayElement(0);
  }

  private static int cells(int n, int d) {
    long cells = (long) n * d;
    if (cells > Integer.MAX_VALUE - 8) throw new Values.ConversionException("an array of " + n + " x " + d + " numbers is too large");
    return (int) cells;
  }

  /** JavaScript's side of bulkArray: [dtype, ArrayBuffer, rows, columns] or null. */
  private Value jsBulk;

  private Value jsBulk() {
    if (jsBulk == null)
      jsBulk = context().eval("js", """
          (() => {
            const typed = [[Float32Array, 'float32'], [Float64Array, 'float64'], [Int8Array, 'int8'],
              [Int16Array, 'int16'], [Int32Array, 'int32'], [Uint8Array, 'uint8'], [Uint16Array, 'uint16'],
              [Uint32Array, 'uint32'], [BigInt64Array, 'int64'], [BigUint64Array, 'uint64']];
            const numbers = (a) => Array.isArray(a) && a.length > 0 && a.every((x) => typeof x === 'number');
            const ints = (a) => a.every(Number.isSafeInteger);
            return (v) => {
              for (const [T, dtype] of typed)
                if (v instanceof T) return [dtype, v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength), v.length, -1];
              if (numbers(v))
                return ints(v) ? ['int64', BigInt64Array.from(v, BigInt).buffer, v.length, -1]
                    : ['float64', Float64Array.from(v).buffer, v.length, -1];
              if (Array.isArray(v) && v.length > 0 && v.every((r) => numbers(r) && r.length === v[0].length)) {
                const n = v.length, d = v[0].length, whole = v.every(ints);
                const out = whole ? new BigInt64Array(n * d) : new Float64Array(n * d);
                for (let i = 0; i < n; i++)
                  for (let j = 0; j < d; j++) out[i * d + j] = whole ? BigInt(v[i][j]) : v[i][j];
                return [whole ? 'int64' : 'float64', out.buffer, n, d];
              }
              return null;
            };
          })()
          """);
    return jsBulk;
  }

  /** Java's side of bulkArray: {dtype, data, rows, columns} for arrays and Lists of numbers. */
  private static Object[] hostNumbers(Object o) {
    return switch (o) {
      case float[] a -> new Object[] {"float32", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case double[] a -> new Object[] {"float64", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case int[] a -> new Object[] {"int32", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case long[] a -> new Object[] {"int64", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case short[] a -> new Object[] {"int16", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case byte[] a -> new Object[] {"int8", java.lang.foreign.MemorySegment.ofArray(a), a.length, -1};
      case float[][] m when rectangular(m) -> {
        int n = m.length, d = m[0].length;
        float[] out = new float[cells(n, d)];
        for (int i = 0; i < n; i++) System.arraycopy(m[i], 0, out, i * d, d);
        yield new Object[] {"float32", java.lang.foreign.MemorySegment.ofArray(out), n, d};
      }
      case double[][] m when rectangular(m) -> {
        int n = m.length, d = m[0].length;
        double[] out = new double[cells(n, d)];
        for (int i = 0; i < n; i++) System.arraycopy(m[i], 0, out, i * d, d);
        yield new Object[] {"float64", java.lang.foreign.MemorySegment.ofArray(out), n, d};
      }
      case java.util.List<?> l when !l.isEmpty() -> boxedNumbers(l.toArray());
      case Object[] a when a.length > 0 -> boxedNumbers(a);
      default -> null;
    };
  }

  private static boolean rectangular(Object[] m) {
    if (m.length == 0 || m[0] == null) return false;
    int d = java.lang.reflect.Array.getLength(m[0]);
    if (d == 0) return false;
    for (Object r : m)
      if (r == null || java.lang.reflect.Array.getLength(r) != d) return false;
    return true;
  }

  /** Boxed numbers: int64 if all are integers that fit, else float64; null if not all numbers. */
  private static Object[] boxedNumbers(Object[] a) {
    boolean whole = true;
    for (Object x : a) {
      if (!(x instanceof Number)) return null;
      whole &= x instanceof Long || x instanceof Integer || x instanceof Short || x instanceof Byte;
    }
    if (whole) {
      long[] out = new long[a.length];
      for (int i = 0; i < a.length; i++) out[i] = ((Number) a[i]).longValue();
      return new Object[] {"int64", java.lang.foreign.MemorySegment.ofArray(out), a.length, -1};
    }
    double[] out = new double[a.length];
    for (int i = 0; i < a.length; i++) out[i] = ((Number) a[i]).doubleValue();
    return new Object[] {"float64", java.lang.foreign.MemorySegment.ofArray(out), a.length, -1};
  }

  /**
   * import_js(name) / Mle.importJs: an npm package (Phase 6) for any language: require() for
   * CommonJS, else import() of an ES module.
   */
  Value importJs(String name) {
    Context c = context();
    // An ES module (or no such CommonJS module) goes to import(); a CommonJS module's own failure
    // while it loads is the routine's to see.
    Value required = c.eval("js", """
        (n) => {
          try { return require(n) } catch (e) {
            const text = String(e && e.message)
            if (typeof require !== 'function' || e instanceof SyntaxError
                || /Cannot (find|load)( CommonJS)? module|ERR_REQUIRE_ESM|ERR_MODULE_NOT_FOUND/.test(text)) return null
            throw e
          }
        }""").execute(name);
    if (!required.isNull()) return required;
    return jsModule(name);
  }

  /** Python's helpers and SQL API, made when the first Python routine is loaded. */
  private void pythonReady() {
    if (python != null) return;
    if (!PYTHON)
      throw new IllegalStateException("Python is not installed in this MLE (package percona-mle-python)");
    Context c = context();
    // All or nothing: an interrupt (KILL) in between leaves Python to be set up again.
    Helpers h = pythonHelpers(c);
    c.getBindings("python").getMember("_mle_helpers").putHashEntry("bulk",
        (org.graalvm.polyglot.proxy.ProxyExecutable) a -> bulkArray(a[0]));
    // Python's rows take row['label'] (exp note 41): a Python class registered for their proxy class.
    pythonHelper0(c, "register_row").execute(c.asValue(GuestSql.GPyRow.class).getMember("static"));
    pySql = new PySql(c, options, a -> libraryModule(a[0].asString(), a[1].asString(), false),
        a -> importJs(a[0].asString()));
    pySql.zone(options.zoneName);
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
    SOURCE_LINES.clear();
    vectorBuffers.clear();
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
        SOURCE_LINES.put(name, source.split("\n", -1));
        String[] names = params.stream().map(JavaRoutines.Param::name).toArray(String[]::new);
        fn = pythonBodyLoader().execute(source, name, names, outs.length > 0, mleApi);
        if (check) checkPython(source, params, imports);
      } else {
        pythonSources.add(name + ".py");
        SOURCE_LINES.put(name + ".py", source.split("\n", -1));
        fn = h.loader().execute(source, name, entry, mleApi);
        if (check) checkPython(source, List.of(), imports);
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

  /**
   * How a Python or JavaScript argument of the tag is made from its host form. Python's DECIMAL
   * argument is always a Decimal (decimal_type is for SQL results, Appendix B.2), its TIMESTAMP an
   * aware datetime in the zone of OPTIONS (B.4).
   */
  private Value converter(Helpers h, int tag, String language) {
    boolean python = language.equals("python");
    return switch (tag) {
      case Wire.T_BYTES -> h.bytes();
      case Wire.T_VECTOR -> h.vector();
      case Wire.T_JSON -> h.json();
      // Python's are made from their fields in invoke.
      case Wire.T_DATE -> python ? null : h.date();
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> python ? null : h.datetime();
      case Wire.T_TIME -> h.time();
      case Wire.T_DECIMAL -> h.decimal();
      // BIGINT UNSIGNED beyond 2^63 comes as a string (hostForm): Python makes it an int.
      case Wire.T_U64 -> python ? h.bigint() : null;
      case Wire.T_I64 -> null;
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
      if (!js) {
        switch (neutral[i]) {
          case Values.VectorRef ref -> {
            Object a = vectorArgument(i, ref);
            if (a != null) {
              args[i] = a;
              continue;
            }
          }
          // A date or naive datetime, or an aware datetime in the zone of OPTIONS (None for the
          // zero timestamp), built from its fields.
          case Values.Temporal t when t.tag() != Wire.T_TIME -> {
            args[i] = pySql.civil(t, true);
            continue;
          }
          case Values.Timestamp t -> {
            args[i] = t.zero() ? null : pySql.timestamp(t);
            continue;
          }
          case null, default -> {}
        }
      }
      Object v = hostForm(neutral[i], js, options.times);
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
        throw new Values.ConversionException(SqlBridge.dispatcher != null && SqlBridge.dispatcher.depth() > 0
            ? "an async JavaScript routine (one that uses await) cannot run while another routine of "
                + "this worker runs (a nested call); its promise did not settle"
            : "the routine's promise did not settle");
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
      // Bounded: many dimensions passed row by row would otherwise keep one array each.
      if (vectorBuffers.size() >= 64) vectorBuffers.clear();
      Value t = pythonHelper("numpy_buffer").execute(ref.dims(), true);
      if (t.isNull()) return null;
      buf = new Object[] {t.getArrayElement(0),
          java.lang.foreign.MemorySegment.ofAddress(t.getArrayElement(1).asLong()).reinterpret(ref.dims() * 4L)};
      vectorBuffers.put(key, buf);
    }
    ((java.lang.foreign.MemorySegment) buf[1]).copyFrom(ref.data());
    return buf[0];
  }

  private static Value pythonHelper0(Context c, String name) {
    return c.getBindings("python").getMember("_mle_helpers").getHashValue(name);
  }

  Value pythonHelper(String name) {
    pythonReady();
    return context.getBindings("python").getMember("_mle_helpers").getHashValue(name);
  }

  /** Pre-warming (mle.pool_prewarm): Python, and numpy too. */
  void prewarmPython(boolean numpy) {
    if (!PYTHON) return;
    pythonReady();
    if (numpy) context().eval("python", "import numpy");
  }

  /** The neutral value in the form the language's converter takes. */
  private static Object hostForm(Object v, boolean js, ZoneCache zone) {
    return switch (v) {
      case null -> null;
      // A JS number; beyond 2^53 a string (Oracle's UNSAFE_STRING default). Passed as a Long,
      // so that integers stay integers when a routine passes them back to SQL.
      case Long l -> js && (l > (1L << 53) - 1 || l < -((1L << 53) - 1)) ? l.toString() : (Object) l;
      case BigInteger b -> b.toString();
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
        // JavaScript: in the session's time zone, as results are read back (toJava) and as the body
        // form does (JsOracle.hostForm): an identity function returns its argument (independent
        // review C16); Oracle's rules for zero and impossible dates (Values.Temporal.jsMillis).
        // (Python's are made in invoke.)
        default -> t.jsMillis(zone);
      };
      // An instant: a JavaScript Date (invalid for the zero timestamp). (Python's are made in invoke.)
      case Values.Timestamp t -> t.zero() ? Double.NaN : (double) t.millis();
      default -> v;  // Double, Float, String
    };
  }

  private static String formatTime(Values.Temporal t) {
    return String.format("%s%02d:%02d:%02d%s", t.negative() ? "-" : "", t.hour(), t.minute(), t.second(),
        t.micro() == 0 ? "" : String.format(".%06d", t.micro()));
  }

  private static final Pattern TIME = Pattern.compile("(-)?(\\d+):(\\d{2}):(\\d{2})(?:\\.(\\d{1,6}))?");

  /** A guest result as a Java value of the declared SQL type. */
  private Object toJava(Value v, Values.ParamType t, Helpers h) {
    if (v == null || v.isNull()) return null;
    // Python's True and False are numbers (1 and 0) for numeric types.
    if (v.isBoolean()) {
      switch (t.tag()) {
        case Wire.T_I64, Wire.T_U64, Wire.T_DECIMAL -> {
          return v.asBoolean() ? 1L : 0L;
        }
        case Wire.T_F64, Wire.T_F32 -> {
          return v.asBoolean() ? 1.0 : 0.0;
        }
        default -> {}
      }
    }
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
        if (v.fitsInBigInteger()) return v.asBigInteger();  // exact: not through a double
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
        if (v.isInstant()) {
          java.time.Instant i = v.asInstant();
          return options.times.localDateTime(i.getEpochSecond(), i.getNano()).toLocalDate();
        }
        if (v.isDate()) return v.asDate();
        throw new Values.ConversionException("cannot convert " + v + " to DATE");
      }
      case Wire.T_DATETIME -> {
        if (v.isDate() && v.isTime() && !v.isTimeZone()) return LocalDateTime.of(v.asDate(), v.asTime());
        // Python's aware datetime is an instant: it goes as TIMESTAMP (§7.2, Appendix B.4). A
        // JavaScript Date for DATETIME is its local time in the zone of OPTIONS (Oracle's table, as
        // JsOracle converts it).
        if (v.isInstant()) {
          java.time.Instant i = v.asInstant();
          return h == js ? options.times.localDateTime(i.getEpochSecond(), i.getNano()) : i;
        }
        if (v.isDate()) return v.asDate();
        throw new Values.ConversionException("cannot convert " + v + " to " + t.sqlName());
      }
      case Wire.T_TIMESTAMP -> {
        // An instant as it is (a JavaScript Date, an aware datetime); a naive datetime goes as
        // DATETIME (Values.writeAs).
        if (v.isInstant()) return v.asInstant();
        if (v.isDate() && v.isTime()) return LocalDateTime.of(v.asDate(), v.asTime());
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
    // Not normalised: 00:60:00 is not a TIME (and more than 838 hours is out of range anyway).
    if (m.group(2).length() > 4 || Integer.parseInt(m.group(3)) > 59 || Integer.parseInt(m.group(4)) > 59)
      throw new Values.ConversionException("Cannot convert value '" + s + "' to TIME from MLE");
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

  /**
   * Python routines' source lines by source name, for tracebacks (exp note 39): their code is
   * compiled by Python from a string, so the frames' sources carry no characters.
   */
  private static final Map<String, String[]> SOURCE_LINES = new java.util.concurrent.ConcurrentHashMap<>();

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
      String text = "";
      try {
        if (src.hasCharacters() && line >= 1 && line <= src.getLineCount())
          text = src.getCharacters(line).toString();
        // The first line starts with MLE's wrapper: the routine's text comes after it.
        Integer prefix = line == 1 && src.getURI() != null ? prefixes.get(src.getURI()) : null;
        if (prefix != null && prefix <= text.length()) text = text.substring(prefix);
      } catch (RuntimeException ignored) {
        // no text: the location alone
      }
      if (text.isEmpty()) {
        String[] lines = SOURCE_LINES.get(src.getName());
        if (lines != null && line >= 1 && line <= lines.length) text = lines[line - 1];
      }
      frames.add(new Dispatcher.Frame(root, src.getName(), line, bodyColumn(src, line, loc.getStartColumn()),
          loc.getEndLine() == line ? bodyColumn(src, line, loc.getEndColumn()) : loc.getEndColumn(),
          Dispatcher.Frame.line(text)));
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
