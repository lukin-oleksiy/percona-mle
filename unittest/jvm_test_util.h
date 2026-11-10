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

// Helpers shared by the JVM worker tests (java_worker-t.cc,
// polyglot_worker-t.cc).

#ifndef MLE_JVM_TEST_UTIL_H
#define MLE_JVM_TEST_UTIL_H

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mle/codec.h"
#include "mle/defaults.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_test {

using namespace mle;

inline std::string dir_of(const std::string &path) {
  return path.substr(0, path.rfind('/'));
}

/** The GraalPy venv with numpy for the tests, or nullptr. */
inline const char *python_venv() {
  static const std::string v = MLE_TEST_PYTHON_VENV;
  return v.empty() ? nullptr : v.c_str();
}

/** The build tree's MLE files (as the installed layout, see defaults.h).
    MLE_TEST_NO_AOT=1 runs workers without the AOT cache. */
inline Layout build_layout() {
  Layout l;
  l.launcher_exe = MLE_LAUNCHER_EXE;
  l.worker_exe = MLE_WORKER_EXE;
  l.worker_jar = MLE_WORKER_JAR;
  l.resource_cache = MLE_RESOURCE_CACHE;
  l.python_venv = python_venv() ? python_venv() : "";
  if (getenv("MLE_TEST_NO_AOT") == nullptr) l.aot_cache = MLE_AOT_CACHE;
  l.java_home = MLE_JAVA_HOME;
  return l;
}

/** The default JVM worker class (defaults.h) for the tests. prewarm: a comma
    list of java, js, python, numpy (see Main). */
inline ClassConfig jvm_class(const std::string &name,
                             const std::string &prewarm = "java",
                             const std::string &heap = "512m") {
  JvmClassOptions o;
  o.name = name;
  o.prewarm = prewarm;
  o.heap = heap;
  o.inherit_stderr = getenv("MLE_TEST_STDERR") != nullptr;
  if (const char *extra = getenv("MLE_TEST_JVM_OPT"))
    o.extra_jvm_options.push_back(extra);
  if (const char *spin = getenv("MLE_TEST_SPIN"))
    o.extra_jvm_options.push_back(std::string("-Dmle.spin=") + spin);
  return jvm_worker_class(build_layout(), o);
}

// ---------------------------------------------------------------- test host

/** The host's Worker with test hooks; host-side errors are put into the
    result's error so that tests can print them uniformly. */
class TestHost {
 public:
  using SqlHandler = std::function<void(const SqlMsg &, SqlResultMsg *)>;

  explicit TestHost(std::unique_ptr<WorkerProcess> w, WorkerOptions o = {})
      : worker(std::move(w), o) {}

  bool wait_ready(ReadyMsg *m, std::string *err, int timeout_ms = 60000) {
    return worker.wait_ready(timeout_ms, m, err) == Status::kOk;
  }
  const wire::RegionHeader &header() { return *worker.process().header(); }

  uint32_t prepare(const PrepareMsg &p, ResultMsg *res) {
    std::string err;
    const Status s = worker.prepare(p, hooks, res, &err);
    if (!err.empty() && !res->error) res->error = Error{"host", err, {}, 0, ""};
    return uint32_t(s);
  }

  /** sql serves the routine's SQL (kept for nested calls). */
  uint32_t call(uint64_t routine, const std::vector<Value> &args,
                ResultMsg *res, SqlHandler sql = nullptr) {
    if (sql && worker.depth() == 0) hooks.handler = std::move(sql);
    std::string err;
    const Status s = worker.call(routine, 0, args, hooks, res, &err);
    if (!err.empty() && !res->error) res->error = Error{"host", err, {}, 0, ""};
    return uint32_t(s);
  }

  /** INTERRUPT for a given call sequence, sent directly (to test the
      worker's handling of an interrupt that precedes its call). */
  void raw_interrupt(uint64_t call_seq) {
    unsigned char b[9];
    b[0] = uint8_t(wire::Control::kInterrupt);
    memcpy(b + 1, &call_seq, 8);
    EXPECT_EQ(send(worker.process().control_fd(), b, sizeof b, MSG_NOSIGNAL),
              9);
  }

  struct Hooks : CallHooks {
    SqlHandler handler;
    std::atomic<bool> kill{false};
    void sql(const SqlMsg &m, SqlResultMsg *r) override {
      if (handler)
        handler(m, r);
      else
        NoSql().sql(m, r);
    }
    bool killed() override { return kill.load(); }
  } hooks;
  Worker worker;
};

inline std::string error_of(const ResultMsg &r) {
  return r.error ? r.error->message : "";
}

inline ParamType type_of(Tag tag, bool is_unsigned = false) {
  ParamType t;
  t.tag = tag;
  t.is_unsigned = is_unsigned;
  return t;
}

/** PREPAREs a routine with IN parameters of the given types. */
inline uint32_t prepare_routine(TestHost &h, uint64_t id,
                                const std::string &lang,
                                const std::string &entry,
                                const std::string &src,
                                std::vector<ParamType> params,
                                std::optional<ParamType> ret, ResultMsg *res) {
  PrepareMsg p;
  p.routine_id = id;
  p.language = lang;
  p.entry = entry;
  p.source = src;
  for (size_t i = 0; i < params.size(); i++)
    p.params.push_back(
        {"p" + std::to_string(i + 1), wire::ParamMode::kIn, params[i]});
  p.returns = ret;
  return h.prepare(p, res);
}

}  // namespace mle_test

#endif  // MLE_JVM_TEST_UTIL_H
