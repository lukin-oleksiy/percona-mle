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

#include "mle/defaults.h"

#include <glob.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace mle {

namespace {

/** access(F_OK), not stat(): under SELinux mysqld_t may not getattr the workers' executables
    (mle_worker_exec_t; denied without an audit record), and F_OK needs only the path's search
    permissions (exp note 25: the same rule for mle_worker). */
bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

/** The path with symlinks resolved, or the path itself if it does not exist.
    Landlock rules apply to the files symlinks point to, and the AOT cache
    checks the class path it was created with, so workers get real paths. */
std::string resolve(const std::string &path) {
  char buf[PATH_MAX];
  return realpath(path.c_str(), buf) != nullptr ? std::string(buf) : path;
}

std::string dir_of(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

}  // namespace

Layout Layout::installed(const std::string &dir, const std::string &java_home) {
  Layout l;
  l.launcher_exe = resolve(dir + "/mle_launcher");
  l.worker_exe = resolve(dir + "/mle_worker");
  l.worker_jar = resolve(dir + "/mle-worker.jar");
  l.resource_cache = resolve(dir + "/resources");
  // The venv is used only through its site-packages (numpy): its bin/python
  // and pyvenv.cfg point to the GraalPy it was built with, which need not be
  // there where MLE is installed.
  if (glob_t g{}; glob((dir + "/python-venv/venv/lib/python*/site-packages").c_str(),
                       GLOB_ONLYDIR, nullptr, &g) == 0) {
    if (g.gl_pathc > 0) l.python_venv = resolve(dir + "/python-venv/venv");
    globfree(&g);
  }
  if (exists(dir + "/mle.aot")) l.aot_cache = resolve(dir + "/mle.aot");
  if (exists(dir + "/native/libmleworker.so"))
    l.native_lib = resolve(dir + "/native/libmleworker.so");
  if (exists(dir + "/cpython/mle_cpython_worker")) {
    l.cpython_exe = resolve(dir + "/cpython/mle_cpython_worker");
    l.cpython_code = resolve(dir + "/cpython/python");
    if (exists(dir + "/cpython/site-packages"))
      l.cpython_site = resolve(dir + "/cpython/site-packages");
    if (exists(dir + "/cpython/pycache"))
      l.cpython_pycache = resolve(dir + "/cpython/pycache");
  }
  if (glob_t g{}; glob((dir_of(l.worker_jar) + "/deps/python-language-*.jar").c_str(), 0,
                       nullptr, &g) == 0) {
    l.python = g.gl_pathc > 0;
    globfree(&g);
  }
  l.java_home = resolve(java_home);
  return l;
}

ClassConfig jvm_worker_class(const Layout &layout, const JvmClassOptions &o) {
  ClassConfig c;
  c.name = o.name;
  c.executable = layout.worker_exe;
  c.args = {
      "--jvm=" + layout.java_home + "/lib/server/libjvm.so",
      "-Djava.class.path=" + layout.worker_jar,
      "-Dpolyglot.engine.userResourceCache=" + layout.resource_cache,
      "-XX:+UseSerialGC",
      // Java code is compiled by C2; GraalJS and GraalPy keep Graal (libgraal)
      // through Truffle. Under concurrent load libgraal kept compiling the
      // workers' Java code (35-55 CPU-s per 10 s over 8 workers, more than
      // the routines): with C2, P4 at 8 clients -17 % p50, -47 % server CPU
      // per query, workers ~45 MiB smaller (exp note 49).
      "-XX:-UseJVMCICompiler",
      // A small initial heap that grows as needed (the JVM's default
      // ratios keep allocation-heavy code fast); the worker trims it with a
      // full GC whenever it is parked: after start (pool) and after RESET
      // (reuse cache). Measured in the 2026-11 optimisation round.
      "-Xms8m",
      "-Xmx" + o.heap,
      "-Xss2m",
      "-XX:CICompilerCount=" + std::to_string(std::max(2, o.compiler_threads)),
      "-Dpolyglot.engine.CompilerThreads=" + std::to_string(o.compiler_threads),
      "-XX:-UsePerfData",
      "--enable-native-access=ALL-UNNAMED",
      "--add-modules=jdk.incubator.vector",
      // The vector helper (exp note 34): its kernels and its top-k insertion
      // compiled on their own, not inlined into the task loops; the first
      // call in a worker then waits less for the JIT (0.9 -> 0.6 s, related
      // items 10k x 768), with the same speed once compiled.
      "-XX:CompileCommand=quiet",
      "-XX:CompileCommand=dontinline,com.percona.mle.api.VectorEngine::tile*",
      "-XX:CompileCommand=dontinline,com.percona.mle.api.VectorEngine::offer*",
      "-XX:CompileCommand=dontinline,com.percona.mle.api.VectorEngine::row*",
      // ThreadWatch: the thread containers list virtual threads too (also
      // in the AOT cache's options, worker/CMakeLists.txt, mle_aot_cache).
      "--add-exports=java.base/jdk.internal.vm=ALL-UNNAMED",
      "-Dmle.class=" + o.name,
      // The worker's memory watcher forces GCs below this limit (exp note 30).
      "-Dmle.memoryMax=" + std::to_string(o.memory_max),
      "-Dmle.vectorThreads=" + std::to_string(o.vector_threads)};
  if (!layout.aot_cache.empty())
    c.args.push_back("-XX:AOTCache=" + layout.aot_cache);
  if (!o.prewarm.empty()) c.args.push_back("-Dmle.prewarm=" + o.prewarm);
  if (!layout.python_venv.empty())
    c.args.push_back("-Dmle.python.venv=" + layout.python_venv);
  if (!layout.package_dir.empty())
    c.args.push_back("-Dmle.packages=" + layout.package_dir);
  c.args.insert(c.args.end(), o.extra_jvm_options.begin(),
                o.extra_jvm_options.end());

  c.env = {"OPENBLAS_NUM_THREADS=" + std::to_string(o.blas_threads),
           "OMP_NUM_THREADS=" + std::to_string(o.blas_threads)};
  if (o.malloc_mmap_threshold > 0)
    c.env.push_back("GLIBC_TUNABLES=glibc.malloc.mmap_threshold=" +
                    std::to_string(o.malloc_mmap_threshold));
  c.exec_paths = {dir_of(layout.worker_exe), layout.java_home};
  c.read_paths = {dir_of(layout.worker_jar), layout.resource_cache};
  if (!layout.python_venv.empty()) c.read_paths.push_back(layout.python_venv);
  if (!layout.package_dir.empty()) c.read_paths.push_back(layout.package_dir);
  if (!layout.aot_cache.empty()) c.read_paths.push_back(layout.aot_cache);
  c.hidden_paths = o.hidden_paths;
  c.memory_max = o.memory_max;
  c.pids_max = o.pids_max;
  c.nofile = 4096;
  c.pool_size = o.pool_size;
  c.inherit_stderr = o.inherit_stderr;
  return c;
}

ClassConfig native_worker_class(const Layout &layout, const JvmClassOptions &o) {
  ClassConfig c = jvm_worker_class(layout, o);
  // The same bootstrap and Main.run; the library is a Native Image with its
  // own language resources beside it (no class path, no AOT cache, no JDK).
  const std::string lib_dir = dir_of(layout.native_lib);
  // Language resources (Python's standard library) as for the JVM class: extracted at build
  // time, since workers cannot write a resource cache (same GraalVM release, same resources).
  c.args = {"--jvm=" + layout.native_lib,
            "-Dpolyglot.engine.userResourceCache=" + layout.resource_cache,
            "-Xmx" + o.heap, "-Dmle.class=" + o.name,
            "-Dmle.memoryMax=" + std::to_string(o.memory_max),
            // The image's GC (exp note 37): the default (adaptive) policy shrinks the
            // young generation under a stream of calls and then runs a full GC
            // (~30 ms) after every young one: 1M calls of a Python function took
            // 1.94 -> 2.17 s. Proportionate: no full GCs (flat ~1.97 s), idle pool
            // workers ~20 MB smaller, the same memory after numpy work.
            "-XX:InitialCollectionPolicy=Proportionate",
            "-Dmle.vectorThreads=" + std::to_string(o.vector_threads)};
  if (!o.prewarm.empty()) c.args.push_back("-Dmle.prewarm=" + o.prewarm);
  if (!layout.python_venv.empty())
    c.args.push_back("-Dmle.python.venv=" + layout.python_venv);
  if (!layout.package_dir.empty())
    c.args.push_back("-Dmle.packages=" + layout.package_dir);
  // GraalPy's home (its standard library) in the extracted resources: the
  // native image does not find it through the resource cache by itself.
  if (glob_t g{}; glob((layout.resource_cache + "/python/python-home/*").c_str(),
                       GLOB_ONLYDIR, nullptr, &g) == 0) {
    if (g.gl_pathc > 0) c.args.push_back(std::string("-Dmle.python.home=") + g.gl_pathv[0]);
    globfree(&g);
  }
  // System properties only (-D...): the -XX options of the JVM class do not
  // apply to a Native Image.
  for (const std::string &x : o.extra_jvm_options)
    if (x.rfind("-D", 0) == 0) c.args.push_back(x);
  c.args.insert(c.args.end(), o.extra_native_options.begin(), o.extra_native_options.end());
  c.exec_paths = {dir_of(layout.worker_exe), lib_dir};
  c.read_paths = {lib_dir, layout.resource_cache};
  if (!layout.python_venv.empty()) c.read_paths.push_back(layout.python_venv);
  if (!layout.package_dir.empty()) c.read_paths.push_back(layout.package_dir);
  return c;
}

ClassConfig cpython_worker_class(const Layout &layout, const JvmClassOptions &o) {
  ClassConfig c = jvm_worker_class(layout, o);  // limits, environment, hidden paths
  c.executable = layout.cpython_exe;
  c.args = {"--class=" + o.name, "--code=" + layout.cpython_code};
  // The standard library is the system's (under /usr/lib or /usr/lib64: the
  // launcher's system paths); the worker knows where from its build.
  c.exec_paths = {dir_of(layout.cpython_exe)};
  c.read_paths = {layout.cpython_code};
  if (!layout.cpython_pycache.empty()) {
    c.args.push_back("--pycache=" + layout.cpython_pycache);
    c.read_paths.push_back(layout.cpython_pycache);
  }
  const auto site = [&c](const std::string &dir) {
    c.args.push_back("--site=" + dir);
    c.read_paths.push_back(dir);
  };
  if (!layout.cpython_site.empty()) site(layout.cpython_site);
  if (!layout.package_dir.empty() && exists(layout.package_dir + "/cpython/site-packages"))
    site(resolve(layout.package_dir + "/cpython/site-packages"));
  if (o.prewarm.find("numpy") != std::string::npos) c.env.push_back("MLE_PREWARM_NUMPY=1");
  return c;
}

}  // namespace mle
