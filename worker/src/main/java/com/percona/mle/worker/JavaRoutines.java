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

  static final class CompileException extends Exception {
    private static final long serialVersionUID = 1L;

    CompileException(String message) {
      super(message);
    }
  }

  record Param(String name, int mode, Values.ParamType type) {}

  /** outs: indexes of OUT and INOUT parameters (passed as one-element arrays). */
  record Routine(long id, MethodHandle invoker, Values.Converter[] converters, String[] names, Values.ParamType ret,
      int[] outs, Values.ParamType[] types) {}

  /** Routine name and form by routine loader name, for stack frames. */
  record Origin(String name, boolean bodyForm) {}

  static final Map<String, Origin> origins = new java.util.concurrent.ConcurrentHashMap<>();

  /** Body form: the class, the method and the line-1 prefix length. */
  private static final String BODY_CLASS = "MleRoutineBody";  // not Routine: com.percona.mle.sql.Routine
  private static final String BODY_METHOD = "call";

  private static final Pattern PACKAGE = Pattern.compile("(?m)^\\s*package\\s+([\\w.]+)\\s*;");
  private static final Pattern PUBLIC_TYPE = Pattern.compile(
      "\\bpublic\\s+(?:(?:final|abstract|sealed|non-sealed|strictfp)\\s+)*(?:class|record|interface|enum)\\s+(\\w+)");

  private final Map<Long, Routine> routines = new HashMap<>();
  private final ClassLoader apiLoader = new ApiLoader(JavaRoutines.class.getClassLoader());
  private JavaCompiler javac;
  private StandardJavaFileManager files;
  private final List<String> options = List.of(
      "-g", "-proc:none", "-encoding", "UTF-8", "-Xlint:none", "-nowarn",
      "--add-modules", "jdk.incubator.vector",
      "-classpath", System.getProperty("java.class.path", ""));

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
    compile("package mle.warm; public class Warm { public static int f(int x) { return x + 1; } }", 0, null);
  }

  Routine get(long id) {
    return routines.get(id);
  }

  void drop(long id) {
    routines.remove(id);
    origins.remove(LOADER_PREFIX + id);
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
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> "java.time.LocalDateTime";
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
    Map<String, byte[]> classes = compile(unit, prefix, bodyForm ? name : null);
    RoutineLoader loader = new RoutineLoader(id, apiLoader, classes);
    origins.put(LOADER_PREFIX + id, new Origin(name, bodyForm));

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
        converters[i] = arrayConverter(type.getComponentType(), params.get(i).mode() == Wire.MODE_OUT);
      } else {
        converters[i] = Values.converter(type);
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
    routines.put(id, new Routine(id, mh, converters, names, ret, outs, types));
  }

  /** OUT/INOUT: a one-element array of the component type; OUT starts as null (0 for primitives). */
  private static Values.Converter arrayConverter(Class<?> component, boolean outOnly) {
    Values.Converter element = Values.converter(component);
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
    Object result = (Object) r.invoker().invokeExact(args);
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

  /**
   * Compiles a unit; prefix: wrapper text before the routine body on line 1 (body form), display:
   * the file name in error messages (null: the class's).
   */
  private Map<String, byte[]> compile(String source, int prefix, String display) throws CompileException {
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
    boolean ok = javac.getTask(null, fm, diags, options, null, List.of(unit)).call();
    if (!ok) {
      StringBuilder msg = new StringBuilder();
      for (Diagnostic<? extends JavaFileObject> d : diags.getDiagnostics()) {
        if (d.getKind() != Diagnostic.Kind.ERROR) continue;
        if (!msg.isEmpty()) msg.append('\n');
        long column = d.getLineNumber() == 1 ? Math.max(1, d.getColumnNumber() - prefix) : d.getColumnNumber();
        msg.append(display != null ? display : typeName + ".java").append(':').append(d.getLineNumber()).append(':').append(column)
            .append(": ").append(d.getMessage(Locale.ROOT));
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
