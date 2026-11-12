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
#include <sys/stat.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace mle {

namespace {

bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

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
      "-Dmle.class=" + o.name};
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

}  // namespace mle
