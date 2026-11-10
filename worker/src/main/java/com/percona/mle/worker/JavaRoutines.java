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
 * <p>A routine's source is a compilation unit with a public class; the entry is
 * [package.]Class.method, a public static method with one parameter per SQL parameter. The routine
 * loader's parent sees only the JDK and com.percona.mle.api. That is tidiness, not security: the
 * worker process is the sandbox.
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

  record Routine(long id, MethodHandle invoker, Values.Converter[] converters, String[] names, Values.ParamType ret) {}

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
      if (name.startsWith("com.percona.mle.api.")) return app.loadClass(name);
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
    compile("package mle.warm; public class Warm { public static int f(int x) { return x + 1; } }");
  }

  Routine get(long id) {
    return routines.get(id);
  }

  void drop(long id) {
    routines.remove(id);
  }

  void reset() {
    routines.clear();
  }

  void prepare(long id, String entry, String source, List<Param> params, Values.ParamType ret)
      throws CompileException {
    Map<String, byte[]> classes = compile(source);
    RoutineLoader loader = new RoutineLoader(id, apiLoader, classes);

    int dot = entry.lastIndexOf('.');
    if (dot <= 0 || dot == entry.length() - 1)
      throw new CompileException("the entry must be Class.method or package.Class.method, not '" + entry + "'");
    String className = entry.substring(0, dot);
    String methodName = entry.substring(dot + 1);
    Class<?> cls = load(loader, className, packageOf(source));

    int n = params.size();
    List<Method> candidates = new ArrayList<>();
    for (Method m : cls.getMethods())
      if (m.getName().equals(methodName) && Modifier.isStatic(m.getModifiers()) && m.getParameterCount() == n)
        candidates.add(m);
    if (candidates.isEmpty())
      throw new CompileException("no public static method " + methodName + " with " + n + " parameter"
          + (n == 1 ? "" : "s") + " in " + cls.getName());
    if (candidates.size() > 1)
      throw new CompileException("more than one method " + methodName + " with " + n + " parameters in " + cls.getName());
    Method m = candidates.get(0);

    Values.Converter[] converters = new Values.Converter[n];
    String[] names = new String[n];
    for (int i = 0; i < n; i++) {
      Class<?> type = m.getParameterTypes()[i];
      converters[i] = Values.converter(type);
      names[i] = params.get(i).name();
      if (converters[i] == null)
        throw new CompileException("parameter " + (i + 1) + " of " + methodName + " has the unsupported type "
            + type.getName());
    }
    MethodHandle mh;
    try {
      mh = MethodHandles.publicLookup().unreflect(m);
    } catch (IllegalAccessException e) {
      throw new CompileException("the class " + cls.getName() + " must be public");
    }
    mh = mh.asType(MethodType.genericMethodType(n)).asSpreader(Object[].class, n);
    routines.put(id, new Routine(id, mh, converters, names, ret));
  }

  /** Converts the neutral arguments and calls the routine. */
  Object invoke(Routine r, Object[] neutral, Arena callArena) throws Throwable {
    Object[] args = new Object[neutral.length];
    for (int i = 0; i < args.length; i++) {
      try {
        args[i] = r.converters()[i].convert(neutral[i], callArena);
      } catch (Values.ConversionException e) {
        throw new Values.ConversionException("argument " + (i + 1) + " (" + r.names()[i] + "): " + e.getMessage());
      }
    }
    return (Object) r.invoker().invokeExact(args);
  }

  // ------------------------------------------------------------ compiler

  private static String packageOf(String source) {
    Matcher m = PACKAGE.matcher(source);
    return m.find() ? m.group(1) : "";
  }

  private static Class<?> load(ClassLoader loader, String name, String pkg) throws CompileException {
    try {
      return Class.forName(name, true, loader);
    } catch (ClassNotFoundException e) {
      if (!pkg.isEmpty() && name.indexOf('.') < 0) {
        try {
          return Class.forName(pkg + "." + name, true, loader);
        } catch (ClassNotFoundException e2) {
          // fall through
        }
      }
      throw new CompileException("class " + name + " not found in the routine source");
    } catch (LinkageError e) {
      throw new CompileException("class " + name + " cannot be loaded: " + e);
    }
  }

  private Map<String, byte[]> compile(String source) throws CompileException {
    if (javac == null) {
      javac = ToolProvider.getSystemJavaCompiler();
      if (javac == null) throw new CompileException("no Java compiler in this JDK (module jdk.compiler)");
      files = javac.getStandardFileManager(null, Locale.ROOT, StandardCharsets.UTF_8);
    }
    Matcher t = PUBLIC_TYPE.matcher(source);
    String typeName = t.find() ? t.group(1) : "Routine";
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
        msg.append(typeName).append(".java:").append(d.getLineNumber()).append(':').append(d.getColumnNumber())
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
