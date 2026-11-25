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

  Where the MLE files are and how the worker classes (JVM, CPython, V8) are
  configured by default. Used by the component, the benchmark harness and
  the tests, so that all of them run workers the same way.

  Installed layout (one directory, e.g. <plugin dir>/mle):
    mle_launcher, mle_worker   executables
    mle-worker.jar, deps/      the worker's Java code and GraalJS/GraalPy
    resources/                 language resources, extracted at build time
    python-venv/               GraalPy venv with numpy
    mle.aot                    JDK AOT cache (optional)
    cpython/                   the CPython worker (optional): mle_cpython_worker,
                               python/ (its code), site-packages/ (numpy)
    v8/                        the V8 worker (optional): mle_v8_worker (the
                               distribution's V8 from libnode; exp note 63)
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
  /** The CPython worker (exp design note 62): <dir>/cpython/mle_cpython_worker;
      empty: not installed. */
  std::string cpython_exe;
  /** Its Python code: <dir>/cpython/python (the package mle_cpython). */
  std::string cpython_code;
  /** Its packages (numpy): <dir>/cpython/site-packages; empty: none. */
  std::string cpython_site;
  /** Bytecode of its code kept apart (a build tree): <dir>/cpython/pycache; empty: none. */
  std::string cpython_pycache;
  /** The V8 worker (exp design note 63): <dir>/v8/mle_v8_worker; empty: not installed. */
  std::string v8_exe;

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
  /** Loaded before READY (comma list of java, js, python, numpy); vectors:
      the vector helper warmed up after READY while the worker is idle. */
  std::string prewarm = "java,js,python";
  uint32_t pool_size = 0;
  /** BLAS threads per worker; many sessions each have a worker. */
  int blas_threads = 1;
  /** glibc's mmap threshold for the worker (GLIBC_TUNABLES), fixed so that
      freed large blocks (numpy arrays) go back to the system at once (exp
      note 45); 0: glibc's adaptive default. */
  uint64_t malloc_mmap_threshold = 0;
  /** Threads of the vector helper (com.percona.mle.api.Vectors, exp note
      33): at most this many per call, never more than the CPUs. */
  uint32_t vector_threads = 4;
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

/** The V8 worker class (exp design note 63; LANGUAGE JS): mle_v8_worker with
    the V8 heap limit (--heap-mb) and the DBA's npm packages
    (<package_dir>/js); limits, environment and hidden paths as for the JVM
    class. */
ClassConfig v8_worker_class(const Layout &layout, const JvmClassOptions &options,
                            uint64_t heap_max);

/** The CPython worker class (exp design note 62; LANGUAGE CPYTHON):
    mle_cpython_worker with its code, its site-packages and the DBA's CPython
    packages (<package_dir>/cpython/site-packages) on sys.path; limits,
    environment and hidden paths as for the JVM class. */
ClassConfig cpython_worker_class(const Layout &layout,
                                 const JvmClassOptions &options = {});

}  // namespace mle

#endif  // MLE_DEFAULTS_H
