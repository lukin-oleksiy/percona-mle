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

#ifndef MLE_DEFAULTS_H
#define MLE_DEFAULTS_H

/**
  @file include/mle/defaults.h

  Where the MLE files are and how a JVM worker class (D-10) is configured
  by default. Used by the component (Phase 4), the benchmark harness and the
  tests, so that all of them run workers the same way.

  Installed layout (one directory, e.g. <plugin dir>/mle):
    mle_launcher, mle_worker   executables
    mle-worker.jar, deps/      the worker's Java code and GraalJS/GraalPy
    resources/                 language resources, extracted at build time
    python-venv/               GraalPy venv with numpy
    mle.aot                    JDK AOT cache (optional)
*/

#include <cstdint>
#include <string>
#include <vector>

#include "mle/launcher.h"

namespace mle {

struct Layout {
  std::string launcher_exe;
  std::string worker_exe;
  std::string worker_jar;
  std::string resource_cache;
  /** The venv directory (with bin/python); empty: Python without packages. */
  std::string python_venv;
  /** Empty: no AOT cache. */
  std::string aot_cache;
  /** The GraalVM JDK 25 that runs the worker. */
  std::string java_home;
  /** Third-party packages (read-only for workers); empty: none. */
  std::string package_dir;
  /** GraalPy is installed (deps/python-language-*.jar; an optional package). */
  bool python = false;
  /** The Python-only native build of the worker (exp design note 23):
      <dir>/native/libmleworker.so, with its language resources beside it;
      empty: not installed. */
  std::string native_lib;

  /** The installed layout below dir; optional parts that do not exist are
      left empty. Symlinks are resolved (the build tree has a layout of
      symlinks for MTR: <build>/plugin_output_directory/mle). */
  static Layout installed(const std::string &dir, const std::string &java_home);
};

struct JvmClassOptions {
  std::string name = "jvm";
  /** cgroup memory.max of each worker: JVM, GraalJS, GraalPy, numpy and
      the page cache they read. A worker with Python and numpy does not fit
      in 1 GiB (W6). */
  uint64_t memory_max = 2ULL << 30;
  /** cgroup pids.max: threads count (JVM, compiler, GraalPy, BLAS). */
  uint32_t pids_max = 512;
  /** Java heap (-Xmx). */
  std::string heap = "512m";
  /** Loaded before READY (comma list of java, js, python, numpy). */
  std::string prewarm = "java,js,python";
  uint32_t pool_size = 0;
  /** BLAS threads per worker; many sessions each have a worker. */
  int blas_threads = 1;
  /** Truffle and C2 compiler threads. */
  int compiler_threads = 2;
  bool inherit_stderr = false;
  std::vector<std::string> extra_jvm_options;
  /** Paths workers must never read (ClassConfig::hidden_paths). */
  std::vector<std::string> hidden_paths;
};

/** The worker class for layout and options: executable, JVM options,
    Landlock paths, environment and limits. */
ClassConfig jvm_worker_class(const Layout &layout,
                             const JvmClassOptions &options = {});

/** The Python-only native worker class (exp design note 23): the same
    bootstrap loads layout.native_lib instead of libjvm.so; system properties
    and the heap size as for the JVM class, no JVM-only options; no JDK in
    the Landlock paths. */
ClassConfig native_worker_class(const Layout &layout,
                                const JvmClassOptions &options = {});

}  // namespace mle

#endif  // MLE_DEFAULTS_H
