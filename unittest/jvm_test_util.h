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
// polyglot_worker-t.cc, worker_failures-t.cc, body_forms-t.cc): the host's
// side is mle::Worker, as docs/protocol.md defines it.

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

inline uint32_t u(Outcome o) { return static_cast<uint32_t>(o); }

/** The SQL type name of a tag, as the host gives it in a Type (§7.4: never
    empty). */
inline const char *sql_type_name(Tag tag, bool is_unsigned = false) {
  switch (tag) {
    case Tag::kNull:
      return "NULL";
    case Tag::kI64:
      return "BIGINT";
    case Tag::kU64:
      return is_unsigned ? "BIGINT" : "BIT";
    case Tag::kF64:
      return "DOUBLE";
    case Tag::kF32:
      return "FLOAT";
    case Tag::kDecimal:
      return "DECIMAL";
    case Tag::kString:
      return "VARCHAR";
    case Tag::kBytes:
      return "VARBINARY";
    case Tag::kDate:
      return "DATE";
    case Tag::kTime:
      return "TIME";
    case Tag::kDatetime:
      return "DATETIME";
    case Tag::kTimestamp:
      return "TIMESTAMP";
    case Tag::kJson:
      return "JSON";
    case Tag::kVector:
      return "VECTOR";
  }
  return "?";
}

inline ParamType type_of(Tag tag, bool is_unsigned = false) {
  ParamType t;
  t.tag = tag;
  t.is_unsigned = is_unsigned;
  t.type_name = sql_type_name(tag, is_unsigned);
  return t;
}

/** An SQL_RESULT ERROR as the server gives one (§10.4); the Worker sets
    error_id. */
inline void sql_error(SqlResultMsg *r, uint32_t sql_errno,
                      const std::string &sqlstate, const std::string &message) {
  *r = SqlResultMsg();
  r->kind = SqlResultKind::kError;
  r->sql_errno = sql_errno;
  r->sqlstate = sqlstate;
  r->message = message;
}

/** An SQL_RESULT ROWS with one complete result set of one batch. */
inline void sql_rows(SqlResultMsg *r, std::vector<Column> columns,
                     std::vector<Value> values) {
  *r = SqlResultMsg();
  r->kind = SqlResultKind::kRows;
  r->columns = std::move(columns);
  r->total_rows = r->columns.empty() ? 0 : values.size() / r->columns.size();
  r->rows = std::move(values);
}

/** The host's Worker with test hooks. A request's outcome is returned as a
    number (the first five are the RESULT's status, §9.4); what the host
    library says about a request that failed without an Error of the worker
    is put into the result's error with the language "host", so that tests
    can print both uniformly. */
class TestHost {
 public:
  using SqlHandler = std::function<void(const SqlMsg &, SqlResultMsg *)>;

  explicit TestHost(std::unique_ptr<WorkerProcess> w, WorkerOptions o = {})
      : worker(std::move(w), o) {}

  /** Waits for READY, then puts options in force (§9.8): no PREPARE, CALL
      or PARSE_LIBRARY goes to a worker without them. */
  bool wait_ready(ReadyMsg *m, std::string *err, int timeout_ms = 60000) {
    if (worker.wait_ready(timeout_ms, m, err) != Outcome::kOk) return false;
    return options(OptionsMsg(), err) == Outcome::kOk;
  }
  Outcome options(const OptionsMsg &o, std::string *err,
                  Error *guest_error = nullptr) {
    return worker.options(o, guest_error, err);
  }
  WorkerStats stats() { return worker.stats(); }

  uint32_t prepare(PrepareMsg p, ResultMsg *res) {
    // The host's guarantees of §9.2.
    if (p.schema.empty()) p.schema = "test";
    p.library_epoch = epoch;
    std::string err;
    const Outcome o = worker.prepare(p, hooks, res, &err);
    if (!err.empty() && !res->error)
      res->error = Error{"host", err, {}, 0, "", 0};
    return u(o);
  }

  /** sql serves the routine's SQL (kept for nested calls). */
  uint32_t call(uint64_t routine, const std::vector<Value> &args,
                ResultMsg *res, SqlHandler sql = nullptr) {
    if (sql && worker.depth() == 0) hooks.handler = std::move(sql);
    std::string err;
    const Outcome o = worker.call(routine, epoch, args, hooks, res, &err);
    if (!err.empty() && !res->error)
      res->error = Error{"host", err, {}, 0, "", 0};
    return u(o);
  }

  struct Hooks : CallHooks {
    SqlHandler handler;
    std::function<void(const LibraryMsg &, LibraryResultMsg *)> on_library;
    std::atomic<bool> kill{false};
    /** If not 0: killed() is true from its kill_at_poll-th call on (counted
        from the moment this was set). */
    std::atomic<int> kill_at_poll{0};
    std::atomic<int> polls{0};
    void sql(const SqlMsg &m, SqlResultMsg *r) override {
      if (handler)
        handler(m, r);
      else
        NoSql().sql(m, r);
    }
    void library(const LibraryMsg &q, LibraryResultMsg *r) override {
      if (on_library)
        on_library(q, r);
      else
        CallHooks::library(q, r);
    }
    bool killed() override {
      const int at = kill_at_poll.load();
      if (at != 0 && ++polls >= at) return true;
      return kill.load();
    }
  } hooks;
  /** The library epoch of the requests (§9.10). */
  uint64_t epoch = 1;
  Worker worker;
};

inline std::string error_of(const ResultMsg &r) {
  return r.error ? r.error->message : "";
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
