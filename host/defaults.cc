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

#include <sys/stat.h>

#include <algorithm>

namespace mle {

namespace {

bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

std::string dir_of(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

}  // namespace

Layout Layout::installed(const std::string &dir, const std::string &java_home) {
  Layout l;
  l.launcher_exe = dir + "/mle_launcher";
  l.worker_exe = dir + "/mle_worker";
  l.worker_jar = dir + "/mle-worker.jar";
  l.resource_cache = dir + "/resources";
  if (exists(dir + "/python-venv/venv/bin/python"))
    l.python_venv = dir + "/python-venv/venv";
  if (exists(dir + "/mle.aot")) l.aot_cache = dir + "/mle.aot";
  l.java_home = java_home;
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
  c.args.insert(c.args.end(), o.extra_jvm_options.begin(),
                o.extra_jvm_options.end());

  c.env = {"OPENBLAS_NUM_THREADS=" + std::to_string(o.blas_threads),
           "OMP_NUM_THREADS=" + std::to_string(o.blas_threads)};
  c.exec_paths = {dir_of(layout.worker_exe), layout.java_home};
  c.read_paths = {dir_of(layout.worker_jar), layout.resource_cache};
  if (!layout.python_venv.empty()) c.read_paths.push_back(layout.python_venv);
  if (!layout.aot_cache.empty()) c.read_paths.push_back(layout.aot_cache);
  c.memory_max = o.memory_max;
  c.pids_max = o.pids_max;
  c.nofile = 4096;
  c.pool_size = o.pool_size;
  c.inherit_stderr = o.inherit_stderr;
  return c;
}

}  // namespace mle
