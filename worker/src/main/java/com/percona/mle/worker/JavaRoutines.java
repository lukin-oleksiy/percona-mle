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

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.foreign.Arena;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.tools.Diagnostic;
import javax.tools.DiagnosticCollector;
import javax.tools.FileObject;
import javax.tools.ForwardingJavaFileManager;
import javax.tools.JavaCompiler;
import javax.tools.JavaFileManager;
import javax.tools.JavaFileObject;
import javax.tools.SimpleJavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;

/**
 * Java routines: compiled in memory with javac at PREPARE, each in its own class loader, called
 * through a MethodHandle.
 *
 * <p>Forms (D-14):
 *
 * <ul>
 *   <li>Method body (CREATE ... LANGUAGE JAVA AS $$ return a + 1; $$, empty entry): wrapped into
 *       {@code public static T call(T1 p1, ...)} of a class; parameter and return types follow the
 *       SQL types (boxed, so NULL works: INT -> Integer, BIGINT -> Long, DECIMAL -> BigDecimal,
 *       DATETIME -> LocalDateTime, VECTOR -> float[], ...). OUT and INOUT parameters are one-element
 *       arrays (as in Oracle's OJVM): assign p[0].
 *   <li>Class (empty entry, the source declares a public class): the entry is its public static
 *       method named like the routine (case-insensitive), else its only public static method with
 *       one parameter per SQL parameter.
 *   <li>Entry given (Phase 3 tests, harness): [package.]Class.method.
 * </ul>
 *
 * <p>The routine loader's parent sees only the JDK, com.percona.mle.api and com.percona.mle.sql. That is tidiness, not
 * security: the worker process is the sandbox. The polyglot API (org.graalvm.polyglot) is visible
 * too, for calls into JS and Python through Mle.context().
 */
final class JavaRoutines {
  static final String LOADER_PREFIX = "mle-routine-";
  /** Loader names of database libraries and DBA jars (ThreadWatch looks for their code). */
  static final String LIBRARY_LOADER_PREFIX = "mle-library-";
  static final String PACKAGE_LOADER = "mle-packages";

  static final class CompileException extends Exception {
    private static final long serialVersionUID = 1L;

    CompileException(String message) {
      super(message);
    }
  }

  record Param(String name, int mode, Values.ParamType type) {}

  /**
   * outs: indexes of OUT and INOUT parameters (passed as one-element arrays), ascending. link:
   * what the routine was compiled from, when it has USING libraries (null otherwise).
   */
  record Routine(long id, MethodHandle invoker, Values.Converter[] converters, String[] names, Values.ParamType ret,
      int[] outs, Values.ParamType[] types, Link link) {}

  /**
   * A routine with USING libraries is linked with the versions of them it was compiled with
   * (protocol §9.10): the PREPARE's fields, to compile it again when a version changes, and each
   * library's version by "schema/name".
   */
  record Link(String schema, String name, String entry, String source, List<Param> params, Values.ParamType ret,
      List<LibraryFileSystem.Import> imports, Map<String, Long> versions) {}

  /** Routine name and form by routine loader name, for stack frames. */
  record Origin(String name, boolean bodyForm, String[] lines) {
    /** Source line n (from 1) of the routine as written, for tracebacks; "" if unknown. */
    String line(int n) {
      return n >= 1 && n <= lines.length ? Dispatcher.Frame.line(lines[n - 1]) : "";
    }
  }

  static final Map<String, Origin> origins = new java.util.concurrent.ConcurrentHashMap<>();

  /** Body form: the class, the method and the line-1 prefix length. */
  private static final String BODY_CLASS = "MleRoutineBody";  // not Routine: com.percona.mle.sql.Routine
  private static final String BODY_METHOD = "call";

  private static final Pattern PACKAGE = Pattern.compile("(?m)^\\s*package\\s+([\\w.]+)\\s*;");
  private static final Pattern PUBLIC_TYPE = Pattern.compile(
      "\\bpublic\\s+(?:(?:final|abstract|sealed|non-sealed|strictfp)\\s+)*(?:class|record|interface|enum)\\s+(\\w+)");

  private final Map<Long, Routine> routines = new HashMap<>();

  /**
   * Prepared routines kept across sessions (exp note 38), by their content (the schema, compiled
   * unit, entry, name, parameters, result and USING libraries' text): a later session of the account
   * gets the same loaded classes, so neither javac nor the JIT starts again. A changed routine or
   * library is another key. Static fields of a routine's own classes therefore live as long as the
   * worker, like those of the DBA's jars.
   */
  private record Cached(Routine routine, String loaderName, Origin origin) {}

  private static final int CACHE_MAX = 64;
  private final Map<String, Cached> cache = new java.util.LinkedHashMap<>(16, 0.75f, true) {
    @Override
    protected boolean removeEldestEntry(Map.Entry<String, Cached> eldest) {
      return size() > CACHE_MAX;
    }
  };
  /** The API, then the DBA's third-party jars (14 §3), for every routine. */
  private final ClassLoader apiLoader = packageLoader(new ApiLoader(JavaRoutines.class.getClassLoader()));
  private JavaCompiler javac;
  private StandardJavaFileManager files;
  private final List<String> options = List.of(
      "-g", "-proc:none", "-encoding", "UTF-8", "-Xlint:none", "-nowarn",
      "--add-modules", "jdk.incubator.vector",
      "-classpath", classPath());

  /** The jars in <mle.packages>/java (sorted, for a stable order). */
  static List<java.nio.file.Path> packageJars() {
    String dir = System.getProperty("mle.packages", "");
    if (dir.isEmpty()) return List.of();
    java.nio.file.Path jarDir = java.nio.file.Path.of(dir, "java");
    if (!java.nio.file.Files.isDirectory(jarDir)) return List.of();
    try (var s = java.nio.file.Files.list(jarDir)) {
      return s.filter(p -> p.toString().endsWith(".jar")).sorted().toList();
    } catch (java.io.IOException e) {
      return List.of();
    }
  }

  private static String classPath() {
    StringBuilder cp = new StringBuilder(System.getProperty("java.class.path", ""));
    for (var jar : packageJars()) cp.append(java.io.File.pathSeparatorChar).append(jar);
    return cp.toString();
  }

  /** The polyglot Context's host class loader: the worker's classes and the DBA's jars. */
  static ClassLoader hostLoader() {
    return packageLoader(JavaRoutines.class.getClassLoader());
  }

  /** The routines' parent loader, shared by Java libraries loaded for other languages. */
  private static final ClassLoader LIBRARY_PARENT = packageLoader(new ApiLoader(JavaRoutines.class.getClassLoader()));
  private static JavaRoutines libraryCompiler;

  /**
   * A Java library for JavaScript and Python (Phase 6): compiled alone into a class loader of its
   * own; returns its top-level classes by simple name.
   */
  static synchronized Map<String, Class<?>> loadLibrary(String name, String source) throws CompileException {
    if (libraryCompiler == null) libraryCompiler = new JavaRoutines();
    Map<String, byte[]> bytes = libraryCompiler.compile(source, 0, name, List.of());
    ClassLoader loader = new ClassLoader(LIBRARY_LOADER_PREFIX + name, LIBRARY_PARENT) {
      @Override
      protected Class<?> findClass(String n) throws ClassNotFoundException {
        byte[] b = bytes.get(n);
        if (b == null) throw new ClassNotFoundException(n);
        return defineClass(n, b, 0, b.length);
      }
    };
    Map<String, Class<?>> classes = new HashMap<>();
    for (String n : bytes.keySet()) {
      if (n.contains("$")) continue;
      try {
        classes.put(n.substring(n.lastIndexOf('.') + 1), loader.loadClass(n));
      } catch (ClassNotFoundException | LinkageError e) {
        throw new CompileException("class " + n + " cannot be loaded: " + e);
      }
    }
    return classes;
  }

  private static ClassLoader packageLoader(ClassLoader api) {
    List<java.nio.file.Path> jars = packageJars();
    if (jars.isEmpty()) return api;
    java.net.URL[] urls = new java.net.URL[jars.size()];
    try {
      for (int i = 0; i < urls.length; i++) urls[i] = jars.get(i).toUri().toURL();
    } catch (java.net.MalformedURLException e) {
      return api;
    }
    return new java.net.URLClassLoader(PACKAGE_LOADER, urls, api);
  }

  /** Loads JDK and the API, nothing of the worker. */
  static final class ApiLoader extends ClassLoader {
    private final ClassLoader app;

    ApiLoader(ClassLoader app) {
      super("mle-api", ClassLoader.getPlatformClassLoader());
      this.app = app;
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
      if (name.startsWith("com.percona.mle.api.") || name.startsWith("com.percona.mle.sql.")
          || name.startsWith("org.graalvm.polyglot."))
        return app.loadClass(name);
      return super.loadClass(name, resolve);
    }
  }

  static final class RoutineLoader extends ClassLoader {
    private final Map<String, byte[]> classes;

    RoutineLoader(long id, ClassLoader parent, Map<String, byte[]> classes) {
      super(LOADER_PREFIX + id, parent);
      this.classes = classes;
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
      byte[] b = classes.get(name);
      if (b == null) throw new ClassNotFoundException(name);
      return defineClass(name, b, 0, b.length);
    }
  }

  /** Loads javac and compiles a trivial class, so that the first PREPARE is fast. */
  void prewarm() throws CompileException {
    compile("package mle.warm; public class Warm { public static int f(int x) { return x + 1; } }", 0, null, List.of());
  }

  Routine get(long id) {
    return routines.get(id);
  }

  void drop(long id) {
    routines.remove(id);
    // A cached routine compiled under this id keeps its loader, and other ids that got it from the
    // cache name their stack frames by it: the origin stays while it is cached (MVP review JVML-9).
    String loader = LOADER_PREFIX + id;
    if (cache.values().stream().noneMatch(c -> c.loaderName().equals(loader))) origins.remove(loader);
  }

  void reset() {
    routines.clear();
    origins.clear();
  }

  /** The Java type of a SQL type in the body form. */
  static String javaType(Values.ParamType t) {
    String name = t.typeName();
    return switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        if (name.equals("BIT")) yield "Long";
        if (t.tag() == Wire.T_U64 || (name.equals("BIGINT") && t.unsigned())) yield "java.math.BigInteger";
        if (name.equals("BIGINT") || name.isEmpty() || (name.equals("INT") && t.unsigned())) yield "Long";
        yield "Integer";  // TINYINT .. INT, YEAR
      }
      case Wire.T_F64 -> "Double";
      case Wire.T_F32 -> "Float";
      case Wire.T_DECIMAL -> "java.math.BigDecimal";
      case Wire.T_BYTES -> "byte[]";
      case Wire.T_DATE -> "java.time.LocalDate";
      case Wire.T_DATETIME -> "java.time.LocalDateTime";
      case Wire.T_TIMESTAMP -> "java.time.Instant";  // an instant (Appendix B.4)
      case Wire.T_TIME -> "java.time.Duration";
      case Wire.T_VECTOR -> "float[]";
      default -> "String";  // strings, ENUM, SET, JSON
    };
  }

  private static final String BODY_IMPORTS =
      "import java.math.*;import java.time.*;import java.util.*;import com.percona.mle.api.*;import com.percona.mle.sql.*;";

  /** The class around a method body; the body starts on line 1 after the returned prefix. */
  private static String[] wrapBody(String body, List<Param> params, Values.ParamType ret) {
    StringBuilder pre = new StringBuilder(BODY_IMPORTS).append("public final class ").append(BODY_CLASS)
        .append("{public static ").append(ret == null ? "void" : javaType(ret)).append(' ').append(BODY_METHOD)
        .append('(');
    for (int i = 0; i < params.size(); i++) {
      Param p = params.get(i);
      pre.append(i == 0 ? "" : ",").append(javaType(p.type())).append(p.mode() == Wire.MODE_IN ? " " : "[] ")
          .append(p.name());
    }
    pre.append("){");
    return new String[] {pre.toString(), "\n}}"};
  }

  void prepare(long id, String routineName, String entry, String source, List<Param> params, Values.ParamType ret)
      throws CompileException {
    prepare(id, "", routineName, entry, source, params, ret, List.of());
  }

  /**
   * With database libraries (USING, Phase 6): Java libraries are compiled with the routine (their
   * classes are the routine's); when a library has another version at a later call, the routine
   * is compiled again before it runs ({@link #linked}).
   */
  void prepare(long id, String schema, String routineName, String entry, String source, List<Param> params,
      Values.ParamType ret, List<LibraryFileSystem.Import> imports) throws CompileException {
    List<String[]> libraries = new ArrayList<>();
    Map<String, Long> versions = new HashMap<>();
    for (LibraryFileSystem.Import i : imports) {
      LibraryFileSystem.Library l;
      try {
        l = LibraryFileSystem.library(i.schema(), i.name());
      } catch (IOException e) {
        throw new CompileException(e.getMessage() == null ? e.toString() : e.getMessage());
      }
      versions.put(i.schema() + "/" + i.name(), l.version());
      // A JavaScript or Python library in USING is not compiled in: the routine reaches it with
      // Mle.importLibrary (USING is what lets it import a library of another schema).
      if (!l.language().equals("java")) continue;
      libraries.add(new String[] {i.schema() + "." + i.name(), new String(l.body(), StandardCharsets.UTF_8)});
    }
    prepareWith(id, schema, routineName, entry, source, params, ret, libraries);
    if (!imports.isEmpty()) {
      Routine r = routines.get(id);
      routines.put(id, new Routine(id, r.invoker(), r.converters(), r.names(), r.ret(), r.outs(), r.types(),
          new Link(schema, routineName, entry, source, params, ret, imports, versions)));
    }
  }

  /**
   * r as it may run in the request being served: each of its USING libraries is resolved for the
   * request (mysqld is asked unless an answer holds), and when one has another version than r was
   * compiled with, r is compiled again and prepared under its id. A refused library or code that
   * no longer compiles fails the call.
   */
  Routine linked(Routine r) throws CompileException {
    Link k = r.link();
    if (k == null) return r;
    boolean same = true;
    for (LibraryFileSystem.Import i : k.imports()) {
      LibraryFileSystem.Library l;
      try {
        l = LibraryFileSystem.library(i.schema(), i.name());
      } catch (IOException e) {
        throw new CompileException(e.getMessage() == null ? e.toString() : e.getMessage());
      }
      Long version = k.versions().get(i.schema() + "/" + i.name());
      same &= version != null && version == l.version();
    }
    if (same) return r;
    prepare(r.id(), k.schema(), k.name(), k.entry(), k.source(), k.params(), k.ret(), k.imports());
    return routines.get(r.id());
  }

  /** CREATE LIBRARY ... LANGUAGE JAVA: compiles the library alone; the error or null. */
  String checkLibrary(String name, String source) {
    try {
      compile(source, 0, name, List.of());
      return null;
    } catch (CompileException e) {
      return e.getMessage();
    }
  }

  private void prepareWith(long id, String schema, String routineName, String entry, String source,
      List<Param> params, Values.ParamType ret, List<String[]> libraries) throws CompileException {
    int n = params.size();
    String name = routineName.isEmpty() ? "routine_" + id : routineName;
    boolean bodyForm = entry.isEmpty() && !PUBLIC_TYPE.matcher(source).find();
    int prefix = 0;
    String unit = source;
    if (bodyForm) {
      String[] wrap = wrapBody(source, params, ret);
      prefix = wrap[0].length();
      unit = wrap[0] + source + wrap[1];
    }
    StringBuilder k = new StringBuilder(unit.length() + 256).append(unit).append('\0').append(entry).append('\0')
        .append(schema).append('\0').append(routineName).append('\0').append(params).append('\0').append(ret);
    for (String[] l : libraries) k.append('\0').append(l[0]).append('\0').append(l[1]);
    String key = k.toString();
    Cached hit = noCache ? null : cache.get(key);
    if (hit != null) {
      Routine r = hit.routine();
      routines.put(id, new Routine(id, r.invoker(), r.converters(), r.names(), r.ret(), r.outs(), r.types(), null));
      origins.put(hit.loaderName(), hit.origin());
      return;
    }
    Map<String, byte[]> classes = compile(unit, prefix, bodyForm ? name : null, libraries);
    RoutineLoader loader = new RoutineLoader(id, apiLoader, classes);
    // The unit's line n is line n of the source as written (the body form's prefix is on line 1).
    Origin origin = new Origin(name, bodyForm, source.split("\n", -1));
    origins.put(LOADER_PREFIX + id, origin);

    String className;
    String methodName;
    if (bodyForm) {
      className = BODY_CLASS;
      methodName = BODY_METHOD;
    } else if (entry.isEmpty()) {
      Matcher t = PUBLIC_TYPE.matcher(source);
      t.find();
      className = t.group(1);
      methodName = null;  // by routine name, else the only candidate
    } else {
      int dot = entry.lastIndexOf('.');
      if (dot <= 0 || dot == entry.length() - 1)
        throw new CompileException("the entry must be Class.method or package.Class.method, not '" + entry + "'");
      className = entry.substring(0, dot);
      methodName = entry.substring(dot + 1);
    }
    Class<?> cls = load(loader, className, packageOf(source));

    List<Method> candidates = new ArrayList<>();
    for (Method m : cls.getMethods())
      if (Modifier.isStatic(m.getModifiers()) && m.getParameterCount() == n
          && (methodName == null || m.getName().equals(methodName)))
        candidates.add(m);
    if (methodName == null && candidates.size() > 1) {
      List<Method> named = candidates.stream().filter(m -> m.getName().equalsIgnoreCase(routineName)).toList();
      if (!named.isEmpty()) candidates = new ArrayList<>(named);
    }
    String wanted = methodName == null ? "public static method" : "public static method " + methodName;
    if (candidates.isEmpty())
      throw new CompileException("no " + wanted + " with " + n + " parameter" + (n == 1 ? "" : "s") + " in "
          + cls.getName());
    if (candidates.size() > 1)
      throw new CompileException("more than one " + wanted + " with " + n + " parameters in " + cls.getName()
          + (methodName == null ? "; name the entry method like the routine" : ""));
    Method m = candidates.get(0);

    Values.Converter[] converters = new Values.Converter[n];
    String[] names = new String[n];
    int[] outs = java.util.stream.IntStream.range(0, n).filter(i -> params.get(i).mode() != Wire.MODE_IN).toArray();
    for (int i = 0; i < n; i++) {
      Class<?> type = m.getParameterTypes()[i];
      boolean out = params.get(i).mode() != Wire.MODE_IN;
      names[i] = params.get(i).name();
      if (out) {
        if (!type.isArray())
          throw new CompileException("parameter " + (i + 1) + " (" + names[i] + ") is OUT or INOUT: its Java type "
              + "must be an array (the value is element 0), not " + type.getName());
        converters[i] = arrayConverter(type.getComponentType(), params.get(i).mode() == Wire.MODE_OUT,
            params.get(i).type());
      } else {
        converters[i] = Values.converter(type, params.get(i).type());
      }
      if (converters[i] == null)
        throw new CompileException("parameter " + (i + 1) + " of " + m.getName() + " has the unsupported type "
            + type.getName());
    }
    MethodHandle mh;
    try {
      mh = MethodHandles.publicLookup().unreflect(m);
    } catch (IllegalAccessException e) {
      throw new CompileException("the class " + cls.getName() + " must be public");
    }
    mh = mh.asType(MethodType.genericMethodType(n)).asSpreader(Object[].class, n);
    Values.ParamType[] types = params.stream().map(Param::type).toArray(Values.ParamType[]::new);
    Routine routine = new Routine(id, mh, converters, names, ret, outs, types, null);
    routines.put(id, routine);
    if (!noCache) cache.put(key, new Cached(routine, LOADER_PREFIX + id, origin));
  }

  /** mle_eval's snippets are not kept (exp note 39). */
  private boolean noCache;

  /**
   * mle_eval (exp note 39): a snippet as a body-form routine returning its value as text. Tried
   * as an expression, then as statements (no value), then as statements with their own return;
   * the code starts on line 1 in each, so line numbers are the snippet's.
   */
  void prepareEval(long id, String code) throws CompileException {
    Values.ParamType text = new Values.ParamType(Wire.T_STRING, false, 0, 0, 0, "LONGTEXT");
    String[] forms = {
        "return String.valueOf((Object) (" + code + "\n));",
        code + "\nreturn null;",
        "return String.valueOf(new Object() { Object run() { " + code + "\n} }.run());"};
    CompileException first = null, statements = null;
    noCache = true;
    try {
      for (int i = 0; i < forms.length; i++) {
        try {
          prepareWith(id, "", "mle_eval", "", forms[i], List.of(), text, List.of());
          return;
        } catch (CompileException e) {
          if (i == 0) first = e;
          if (i == 1) statements = e;
        }
      }
    } finally {
      noCache = false;
    }
    // The likely intent: one expression (no ';'), else statements.
    throw code.contains(";") ? statements : first;
  }

  /** Drops the kept routine with r's code (its classes are unusable). */
  private void forgetCached(Routine r) {
    cache.values().removeIf(c -> c.routine().invoker() == r.invoker());
  }

  /** OUT/INOUT: a one-element array of the component type; OUT starts as null (0 for primitives). */
  private static Values.Converter arrayConverter(Class<?> component, boolean outOnly, Values.ParamType t) {
    Values.Converter element = Values.converter(component, t);
    if (element == null) return null;
    return (v, a) -> {
      Object array = java.lang.reflect.Array.newInstance(component, 1);
      if (!outOnly && v != null) java.lang.reflect.Array.set(array, 0, element.convert(v, a));
      return array;
    };
  }

  /** Converts the neutral arguments and calls the routine; OUT values go to outs. */
  Object invoke(Routine r, Object[] neutral, Arena callArena, Object[] outs) throws Throwable {
    Object[] args = new Object[neutral.length];
    for (int i = 0; i < args.length; i++) {
      try {
        args[i] = r.converters()[i].convert(neutral[i], callArena);
      } catch (Values.ConversionException e) {
        throw new Values.ConversionException("argument " + (i + 1) + " (" + r.names()[i] + "): " + e.getMessage());
      }
    }
    Object result;
    try {
      result = (Object) r.invoker().invokeExact(args);
    } catch (ExceptionInInitializerError | NoClassDefFoundError e) {
      // A class of the routine failed to initialise: the JVM keeps it failed for good, so the
      // kept routine would fail in every later session (exp note 38). Forget it; the next
      // PREPARE compiles it again.
      forgetCached(r);
      throw e;
    }
    for (int i : r.outs()) outs[i] = java.lang.reflect.Array.get(args[i], 0);
    return result;
  }

  // ------------------------------------------------------------ compiler

  private static String packageOf(String source) {
    Matcher m = PACKAGE.matcher(source);
    return m.find() ? m.group(1) : "";
  }

  private static Class<?> load(ClassLoader loader, String name, String pkg) throws CompileException {
    // Not initialized here: static initializers run at the first call, as
    // usual in Java, so no routine code runs during PREPARE.
    try {
      return Class.forName(name, false, loader);
    } catch (ClassNotFoundException e) {
      if (!pkg.isEmpty() && name.indexOf('.') < 0) {
        try {
          return Class.forName(pkg + "." + name, false, loader);
        } catch (ClassNotFoundException e2) {
          // fall through
        }
      }
      throw new CompileException("class " + name + " not found in the routine source");
    } catch (LinkageError e) {
      throw new CompileException("class " + name + " cannot be loaded: " + e);
    }
  }

  /** A compilation unit for javac: the path of its public type. */
  private static JavaFileObject unit(String source, String fallbackType) {
    Matcher t = PUBLIC_TYPE.matcher(source);
    String typeName = t.find() ? t.group(1) : fallbackType;
    String pkg = packageOf(source);
    String path = (pkg.isEmpty() ? "" : pkg.replace('.', '/') + "/") + typeName;
    return new SimpleJavaFileObject(URI.create("string:///" + path + ".java"), JavaFileObject.Kind.SOURCE) {
      @Override
      public CharSequence getCharContent(boolean ignoreEncodingErrors) {
        return source;
      }
    };
  }

  private void discardCompiler() {
    if (files != null) {
      try {
        files.close();
      } catch (java.io.IOException | RuntimeException e) {
        // closing a broken file manager: nothing to do
      }
    }
    files = null;
    javac = null;
  }

  /**
   * Compiles a unit with the routine's Java libraries ({name, source}); prefix: wrapper text before
   * the routine body on line 1 (body form), display: the file name in error messages (null: the
   * class's).
   */
  private Map<String, byte[]> compile(String source, int prefix, String display, List<String[]> libraries)
      throws CompileException {
    if (javac == null) {
      javac = ToolProvider.getSystemJavaCompiler();
      if (javac == null) throw new CompileException("no Java compiler in this JDK (module jdk.compiler)");
      files = javac.getStandardFileManager(null, Locale.ROOT, StandardCharsets.UTF_8);
    }
    Matcher t = PUBLIC_TYPE.matcher(source);
    String typeName = t.find() ? t.group(1) : BODY_CLASS;
    String pkg = packageOf(source);
    String path = (pkg.isEmpty() ? "" : pkg.replace('.', '/') + "/") + typeName;

    JavaFileObject unit = new SimpleJavaFileObject(URI.create("string:///" + path + ".java"), JavaFileObject.Kind.SOURCE) {
      @Override
      public CharSequence getCharContent(boolean ignoreEncodingErrors) {
        return source;
      }
    };
    Map<String, ByteArrayOutputStream> out = new HashMap<>();
    JavaFileManager fm = new ForwardingJavaFileManager<JavaFileManager>(files) {
      @Override
      public JavaFileObject getJavaFileForOutput(
          JavaFileManager.Location location, String className, JavaFileObject.Kind kind, FileObject sibling) {
        return new SimpleJavaFileObject(URI.create("mem:///" + className.replace('.', '/') + kind.extension), kind) {
          @Override
          public OutputStream openOutputStream() {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            out.put(className, b);
            return b;
          }
        };
      }
    };
    DiagnosticCollector<JavaFileObject> diags = new DiagnosticCollector<>();
    List<JavaFileObject> units = new ArrayList<>();
    units.add(unit);
    Map<JavaFileObject, String> libraryNames = new HashMap<>();
    for (String[] lib : libraries) {
      JavaFileObject u = unit(lib[1], "Library");
      units.add(u);
      libraryNames.put(u, lib[0]);
    }
    boolean ok;
    try {
      ok = javac.getTask(null, fm, diags, options, null, units).call();
    } catch (RuntimeException e) {
      discardCompiler();
      throw e;
    } finally {
      // A KILL during the compile interrupts this thread, which closes the file manager's jar
      // channels (interruptible I/O): later compiles in this worker would fail. Start afresh.
      if (Thread.currentThread().isInterrupted()) discardCompiler();
    }
    if (!ok) {
      StringBuilder msg = new StringBuilder();
      for (Diagnostic<? extends JavaFileObject> d : diags.getDiagnostics()) {
        if (d.getKind() != Diagnostic.Kind.ERROR) continue;
        if (!msg.isEmpty()) msg.append('\n');
        String library = d.getSource() == null ? null : libraryNames.get(d.getSource());
        long column = library == null && d.getLineNumber() == 1 ? Math.max(1, d.getColumnNumber() - prefix)
            : d.getColumnNumber();
        String text = d.getMessage(Locale.ROOT);
        // A body's class is MLE's wrapper: javac's "location: class MleRoutineBody" means the
        // routine (exp note 41).
        if (library == null && typeName.equals(BODY_CLASS))
          text = text.replace("class " + BODY_CLASS, "routine " + (display != null ? display : "body"))
              .replace(BODY_CLASS, display != null ? display : "body");
        msg.append(library != null ? "library " + library : display != null ? display : typeName + ".java")
            .append(':').append(d.getLineNumber()).append(':').append(column)
            .append(": ").append(text);
      }
      throw new CompileException(msg.isEmpty() ? "compilation failed" : msg.toString());
    }
    try {
      fm.flush();
    } catch (IOException e) {
      throw new CompileException(e.toString());
    }
    Map<String, byte[]> classes = new HashMap<>();
    out.forEach((k, v) -> classes.put(k, v.toByteArray()));
    return classes;
  }
}
