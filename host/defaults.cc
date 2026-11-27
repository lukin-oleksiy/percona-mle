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
  if (exists(dir + "/cpython/mle_cpython_worker")) {
    l.cpython_exe = resolve(dir + "/cpython/mle_cpython_worker");
    l.cpython_code = resolve(dir + "/cpython/python");
    if (exists(dir + "/cpython/site-packages"))
      l.cpython_site = resolve(dir + "/cpython/site-packages");
    if (exists(dir + "/cpython/pycache"))
      l.cpython_pycache = resolve(dir + "/cpython/pycache");
  }
  if (exists(dir + "/v8/mle_v8_worker")) l.v8_exe = resolve(dir + "/v8/mle_v8_worker");
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
      "-Dmle.vectorThreads=" + std::to_string(o.vector_threads),
      "-Dmle.idleTrimMs=" + std::to_string(o.idle_trim_ms),
      "-Dmle.vectorCacheMax=" + std::to_string(o.vector_cache_max),
      "-Dmle.vectorCacheIdleMs=" + std::to_string(o.vector_cache_idle_ms)};
  if (!layout.aot_cache.empty()) {
    c.args.push_back("-XX:AOTCache=" + layout.aot_cache);
    // Map the cache at the address it was created for, so its pages stay
    // clean and shared between workers: relocating it (the JDK's default
    // since 21) dirtied ~100 MiB per worker. The JVM still relocates when
    // that address is taken. Costs the cache's ASLR (exp note 62 §11).
    c.args.push_back("-XX:+UnlockDiagnosticVMOptions");
    c.args.push_back("-XX:ArchiveRelocationMode=0");
  }
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

/** --message-mb of a native worker: a host message of more than a quarter of
    the class's memory is not held but failed where it belongs (§5.3); the
    worker holds it once as bytes and once decoded (MVP review CPY-9). */
static void bound_messages(ClassConfig &c, const JvmClassOptions &o) {
  if (o.memory_max == 0) return;
  c.args.push_back("--message-mb=" + std::to_string(std::max<uint64_t>(o.memory_max / 4 >> 20, 16)));
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
  bound_messages(c, o);
  return c;
}

ClassConfig v8_worker_class(const Layout &layout, const JvmClassOptions &o, uint64_t heap_max) {
  ClassConfig c = jvm_worker_class(layout, o);  // limits, environment, hidden paths
  c.executable = layout.v8_exe;
  c.args = {"--class=" + o.name, "--heap-mb=" + std::to_string(heap_max >> 20)};
  // libnode and its libraries are the system's (the launcher's system paths).
  c.exec_paths = {dir_of(layout.v8_exe)};
  c.read_paths = {};
  if (!layout.package_dir.empty() && exists(layout.package_dir + "/js")) {
    const std::string js = resolve(layout.package_dir + "/js");
    c.args.push_back("--packages=" + js);
    c.read_paths.push_back(js);
  }
  // ICU's data of a libnode with ICU of its own (the RHEL family: /usr/share/node-22/icudata),
  // found here: the sandboxed worker cannot list /usr/share.
  if (glob_t g{}; glob("/usr/share/node*/icudata", GLOB_ONLYDIR, nullptr, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      c.args.push_back(std::string("--icu-data=") + g.gl_pathv[i]);
      c.read_paths.push_back(g.gl_pathv[i]);
    }
    globfree(&g);
  }
  // The JVM's environment (BLAS threads, malloc tunables) means nothing to V8.
  c.env.clear();
  bound_messages(c, o);
  return c;
}

}  // namespace mle
