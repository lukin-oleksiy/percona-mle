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

  mle_cpython_worker: the MLE worker for Python routines on CPython (docs/protocol.md; worker
  class cpython, language python).

    mle_cpython_worker --code=<dir> [--site=<dir>]... [--pycache=<dir>] [--class=<name>] [--spin=<n>]
                       [--message-mb=<n>] [--kept-statements=<n>] [--dumpable]

  Started by mle_launcher like mle_worker: Landlock, the cgroup and the descriptors (3 = mailbox,
  4 = control socket) are in place. Here:
  1. PR_SET_DUMPABLE 0 unless --dumpable;
  2. the seccomp filter of mle_worker (worker/bootstrap/seccomp.cc), before any Python code runs;
  3. the mailbox and the control thread (§2 steps 3 and 4);
  4. CPython with an isolated configuration: sys.path = the standard library this program was
     built for (MLE_PYTHON_STDLIB, MLE_PYTHON_DYNLOAD), --code (the worker's package mle_cpython)
     and the --site directories (numpy, DBA packages); no site module, no bytecode writing
     (--pycache: bytecode the build made for code that is not beside its bytecode, as a build
     tree's linked sources); the built-in module _mlewire (module.cc); then
     mle_cpython.worker.main(), which posts READY and serves. If Python or the worker's code
     cannot start, the worker posts a failed READY with the reason (§2 step 5).

  --class is the launcher's name of the worker class, for READY's info. --message-mb: a host
  message of more MiB than this is not held but failed where it belongs (§5.3); 0, the default: as
  large as memory allows. --kept-statements: the statements a routine keeps prepared between its
  calls (§10.1), at most; 0: none.

  Exit codes: 0 normal (the host closed the control socket); 1 a protocol failure, or the worker
  cannot serve (after a failed READY); 2 fd 3 is not an MLE region; 20 bad arguments; 21 dumpable;
  22 seccomp; 23 environment; 25 the worker's Python code failed while serving.
*/

#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bootstrap.h"
#include "mailbox.hpp"
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
    } else if (value("--message-mb=", &v)) {
      mle::cpython::config().message_max = std::strtoull(v.c_str(), nullptr, 10) << 20;
    } else if (value("--kept-statements=", &v)) {
      mle::cpython::config().kept_statements = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else {
      fprintf(stderr, "mle_cpython_worker: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  return !a->code.empty();
}

/** CPython with sys.path as described above; if it cannot start, why. */
std::string start_python(const Args &a) {
  if (PyImport_AppendInittab("_mlewire", mle::cpython::init_mlewire) != 0) return "no memory for the module table";
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 1;  // Python's own (SIGPIPE ignored); interrupts need none (mailbox.cc)
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
  if (PyStatus_Exception(st)) return st.err_msg != nullptr ? st.err_msg : "unknown error";
  return "";
}

/** The Python error that is set, as text for a failed READY. */
std::string python_error() {
  std::string text = "unknown error";
  PyObject *e = PyErr_GetRaisedException();
  if (e != nullptr) {
    if (PyObject *shown = PyObject_ASCII(e); shown != nullptr) {
      if (const char *s = PyUnicode_AsUTF8(shown); s != nullptr) text = s;
      Py_DECREF(shown);
    }
    PyErr_Clear();
    Py_DECREF(e);
  }
  return text;
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse(argc, argv, &args)) {
    fprintf(stderr,
            "usage: mle_cpython_worker --code=<dir> [--site=<dir>]... [--pycache=<dir>] [--class=<name>] [--spin=<n>] "
            "[--message-mb=<n>] [--kept-statements=<n>] [--dumpable]\n");
    return 20;
  }
  if (!args.dumpable && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) return 21;
  if (const int err = mle::worker::install_seccomp(); err != 0) {
    fprintf(stderr, "mle_cpython_worker: seccomp: %s\n", strerror(err));
    return 22;
  }
  if (setenv("MLE_CLASS", args.cls.c_str(), 1) != 0) return 23;
  // From here on a failure to start is told to the host in a failed READY (§2 step 5).
  mle::cpython::config().arena_size = mle::cpython::open_mailbox(args.spin);
  if (const std::string why = start_python(args); !why.empty())
    mle::common::Mailbox::failed_ready("Python did not start: " + why);
  PyObject *mod = PyImport_ImportModule("mle_cpython.worker");
  PyObject *start = mod != nullptr ? PyObject_CallMethod(mod, "start", nullptr) : nullptr;
  if (start == nullptr) mle::common::Mailbox::failed_ready("the worker's Python code did not start: " + python_error());
  // READY is posted by serve(), which returns only if the worker's own code fails.
  PyObject_CallMethod(start, "serve", nullptr);
  PySys_SetObject("stderr", PySys_GetObject("__stderr__"));  // not the routines' console
  PyErr_Print();
  _exit(25);
}
