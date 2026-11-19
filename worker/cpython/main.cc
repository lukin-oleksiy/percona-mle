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

/**
  @file worker/cpython/main.cc

  mle_cpython_worker: the MLE worker for Python routines on CPython (exp design note 62).

    mle_cpython_worker --code=<dir> [--site=<dir>]... [--pycache=<dir>] [--class=<name>] [--spin=<n>]
                       [--dumpable]

  Started by mle_launcher like mle_worker: Landlock, the cgroup and the descriptors (3 = mailbox,
  4 = control socket) are in place. Here:
  1. PR_SET_DUMPABLE 0 unless --dumpable;
  2. the seccomp filter of mle_worker (worker/bootstrap/seccomp.cc), before any Python code runs;
  3. CPython with an isolated configuration: sys.path = the standard library this program was
     built for (MLE_PYTHON_STDLIB, MLE_PYTHON_DYNLOAD), --code (the worker's package mle_cpython)
     and the --site directories (numpy, DBA packages); no site module, no bytecode writing
     (--pycache: bytecode the build made for code that is not beside its bytecode, as a build
     tree's linked sources); the built-in module _mlewire (module.cc); then
     mle_cpython.worker.main().

  Exit codes: 0 normal (the host closed the control socket); 1 fatal protocol error; 20 bad
  arguments; 21 dumpable; 22 seccomp; 23 environment or signals; 24 Python start; 25 the worker's
  Python code failed.
*/

#include <signal.h>
#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bootstrap.h"
#include "module.hpp"

#ifndef MLE_PYTHON_STDLIB
#error "MLE_PYTHON_STDLIB (the standard library directory of the Python this worker embeds) is not set"
#endif
#ifndef MLE_PYTHON_DYNLOAD
#error "MLE_PYTHON_DYNLOAD (its lib-dynload directory) is not set"
#endif

namespace {

struct Args {
  std::string code;
  std::vector<std::string> site;
  std::string pycache;
  std::string cls = "cpython";
  int spin = 2000;
  bool dumpable = false;
};

bool parse(int argc, char **argv, Args *a) {
  for (int i = 1; i < argc; i++) {
    const std::string_view s = argv[i];
    const auto value = [&s](std::string_view prefix, std::string *out) {
      if (!s.starts_with(prefix)) return false;
      *out = s.substr(prefix.size());
      return true;
    };
    std::string v;
    if (s == "--dumpable") {
      a->dumpable = true;
    } else if (value("--code=", &a->code) || value("--class=", &a->cls) || value("--pycache=", &a->pycache)) {
    } else if (value("--site=", &v)) {
      a->site.push_back(v);
    } else if (value("--spin=", &v)) {
      a->spin = std::atoi(v.c_str());
    } else {
      fprintf(stderr, "mle_cpython_worker: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  return !a->code.empty();
}

/** CPython with sys.path as described above; false (and a message) if it cannot start. */
bool start_python(const Args &a) {
  if (PyImport_AppendInittab("_mlewire", mle::cpython::init_mlewire) != 0) return false;
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 1;  // KILL: SIGINT's handler raises KeyboardInterrupt
  config.write_bytecode = 0;           // workers cannot write
  config.site_import = 0;
  config.module_search_paths_set = 1;
  std::vector<std::string> paths = {MLE_PYTHON_STDLIB, MLE_PYTHON_DYNLOAD, a.code};
  paths.insert(paths.end(), a.site.begin(), a.site.end());
  PyStatus st = PyConfig_SetBytesString(&config, &config.program_name, "mle_cpython_worker");
  if (!PyStatus_Exception(st) && !a.pycache.empty())
    st = PyConfig_SetBytesString(&config, &config.pycache_prefix, a.pycache.c_str());
  for (const std::string &p : paths) {
    if (PyStatus_Exception(st)) break;
    wchar_t *wide = Py_DecodeLocale(p.c_str(), nullptr);
    if (wide == nullptr) {
      st = PyStatus_NoMemory();
      break;
    }
    st = PyWideStringList_Append(&config.module_search_paths, wide);
    PyMem_RawFree(wide);
  }
  if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(st)) {
    fprintf(stderr, "mle_cpython_worker: Python did not start: %s\n", st.err_msg ? st.err_msg : "?");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse(argc, argv, &args)) {
    fprintf(stderr,
            "usage: mle_cpython_worker --code=<dir> [--site=<dir>]... [--class=<name>] [--spin=<n>] "
            "[--dumpable]\n");
    return 20;
  }
  if (!args.dumpable && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) return 21;
  if (const int err = mle::worker::install_seccomp(); err != 0) {
    fprintf(stderr, "mle_cpython_worker: seccomp: %s\n", strerror(err));
    return 22;
  }
  if (setenv("MLE_CLASS", args.cls.c_str(), 1) != 0 ||
      setenv("MLE_SPIN", std::to_string(args.spin).c_str(), 1) != 0)
    return 23;
  // KILL raises KeyboardInterrupt through SIGINT's Python handler (PyErr_SetInterruptEx), and
  // CPython installs that handler only where SIGINT has its default action: under systemd mysqld's
  // workers inherit SIGINT ignored (KILL then did nothing until the host's SIGKILL). The worker
  // takes no real SIGINT from anyone (seccomp: no signals across processes but its own).
  struct sigaction dfl {};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigset_t intr;
  sigemptyset(&intr);
  sigaddset(&intr, SIGINT);
  if (sigaction(SIGINT, &dfl, nullptr) != 0 || pthread_sigmask(SIG_UNBLOCK, &intr, nullptr) != 0) return 23;
  if (!start_python(args)) return 24;
  PyObject *mod = PyImport_ImportModule("mle_cpython.worker");
  PyObject *r = mod != nullptr ? PyObject_CallMethod(mod, "main", nullptr) : nullptr;
  if (r == nullptr) {
    PyErr_Print();
    _exit(25);
  }
  // main() returns only if the host is gone; the control thread normally ends the process.
  _exit(0);
}
