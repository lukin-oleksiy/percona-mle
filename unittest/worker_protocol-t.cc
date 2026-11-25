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

// The host's side of the MLE worker protocol (docs/protocol.md), against
// mle_test_worker in its "proto" mode: a worker that does what the scripts
// of these tests say (proto_script.h). Every violation class of §13 must
// end the worker with the violation outcome; what guest code can reach must
// give an error and leave the worker in use; deadlines and the grace clock
// must end a worker when the specification says so, and not before.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "mle/launcher.h"
#include "mle/worker.h"
#include "proto_script.h"

namespace mle_worker_protocol_unittest {

using namespace mle;
using mle_test::Script;
using wire::Op;
using wire::SqlAction;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kArena = wire::kMinArenaSize;
constexpr uint32_t kMore = wire::kMore;
/** The grace period of the timing tests, in ms. */
constexpr int kGrace = 300;

uint32_t u(Op op) { return static_cast<uint32_t>(op); }
uint32_t u(Status s) { return static_cast<uint32_t>(s); }

std::string dir_of(const std::string &path) {
  return path.substr(0, path.rfind('/'));
}

int64_t ms_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               t0)
      .count();
}

class Hooks : public CallHooks {
 public:
  struct Refused {
    SqlRefusal why;
    SqlAction action;
    uint64_t stmt_id;
    /** The action had been given to sql() before. */
    bool ran = false;
    uint64_t error_id = 0;
  };

  void sql(const SqlMsg &q, SqlResultMsg *r) override {
    ran.push_back(q.action);
    if (on_sql)
      on_sql(q, r);
    else
      r->kind = SqlResultKind::kOk;
  }
  void sql_into(const SqlMsg &q, SqlResultMsg *r, DirectReply *,
                uint64_t error_id) override {
    error_ids.push_back(error_id);
    sql(q, r);
  }
  void sql_refused(SqlRefusal why, bool ran, SqlAction action, uint64_t stmt_id,
                   uint64_t error_id, SqlResultMsg *error) override {
    refused.push_back({why, action, stmt_id, ran, error_id});
    CallHooks::sql_refused(why, ran, action, stmt_id, error_id, error);
  }
  bool sql_bytes_reserve(uint64_t bytes) override {
    if (bytes > bytes_bound - bytes_held) return false;
    bytes_held += bytes;
    bytes_peak = std::max(bytes_peak, bytes_held);
    return true;
  }
  void sql_bytes_release(uint64_t bytes) override { bytes_held -= bytes; }
  bool killed() override { return kill.load(); }
  void sleeping() override { ++sleeps; }
  void library(const LibraryMsg &q, LibraryResultMsg *r) override {
    libraries.push_back(q);
    if (on_library)
      on_library(q, r);
    else
      CallHooks::library(q, r);
  }

  std::function<void(const SqlMsg &, SqlResultMsg *)> on_sql;
  std::function<void(const LibraryMsg &, LibraryResultMsg *)> on_library;
  std::vector<SqlAction> ran;
  std::vector<Refused> refused;
  /** The error_id each call of sql_into() was given. */
  std::vector<uint64_t> error_ids;
  std::vector<LibraryMsg> libraries;
  uint64_t bytes_bound = std::numeric_limits<uint64_t>::max();
  uint64_t bytes_held = 0;
  uint64_t bytes_peak = 0;
  std::atomic<bool> kill{false};
  /** Waits that went on past the spin time. */
  int sleeps = 0;
};

// ---------------------------------------------------------------- scripts

ReadyMsg good_ready() {
  ReadyMsg m;
  m.info = "proto";
  m.worker_class = "test";
  m.languages = {"none"};
  m.runtime = "mle_test_worker";
  return m;
}

template <class M>
std::string bytes_of(const M &m) {
  std::string body;
  encode(m, &body);
  return body;
}

/** The RESULT of a function that returns 42. */
ResultMsg good_result() {
  ResultMsg m;
  m.ret = Value::i64(42);
  return m;
}

ResultMsg returning(Value v) {
  ResultMsg m;
  m.ret = std::move(v);
  return m;
}

ResultMsg failed(std::string language = "", std::string message = "boom") {
  ResultMsg m;
  m.error = Error{std::move(language), std::move(message), {}, 0, "", 0};
  return m;
}

/** A RESULT posted raw, with the header fields as given. */
Script raw_result(std::string_view arena, uint32_t status = 0,
                  uint32_t flags = 0, uint32_t len = mle_test::kAuto,
                  uint64_t total = mle_test::kAuto, int32_t depth_delta = 0,
                  int64_t seq_delta = 0, uint32_t reserved = 0) {
  return Script().post(u(Op::kResult), status, flags, arena, len, total,
                       depth_delta, seq_delta, reserved);
}

Script fail_with(Status status, std::string language = "") {
  return Script().result(failed(std::move(language)), u(status));
}

SqlMsg execute(std::string text = "SELECT 1", uint64_t id = 1) {
  SqlMsg m;
  m.action = SqlAction::kExecute;
  m.stmt_id = id;
  m.statement = std::move(text);
  return m;
}

SqlMsg action(SqlAction a, uint64_t id = 1) {
  SqlMsg m;
  m.action = a;
  m.stmt_id = id;
  return m;
}

Script sql(const SqlMsg &m) { return Script().message(Op::kSql, m); }

LibraryMsg library(std::string schema = "db", std::string name = "lib") {
  LibraryMsg m;
  m.schema = std::move(schema);
  m.name = std::move(name);
  return m;
}

/** The READY scripts of the worker classes, by class name. */
std::vector<std::pair<std::string, Script>> ready_classes() {
  std::vector<std::pair<std::string, Script>> c;
  auto ready = [&c](const char *name, const ReadyMsg &m, uint32_t status = 0) {
    c.emplace_back(name, Script().message(Op::kReady, m, status));
  };
  auto raw = [&c](const char *name, std::string_view arena, uint32_t status = 0,
                  uint32_t flags = 0, uint32_t len = mle_test::kAuto,
                  uint64_t total = mle_test::kAuto, int32_t depth = 0,
                  int64_t seq = 0, uint32_t reserved = 0) {
    c.emplace_back(name, Script().post(u(Op::kReady), status, flags, arena, len,
                                       total, depth, seq, reserved));
  };
  const std::string good = bytes_of(good_ready());
  ReadyMsg m = good_ready();
  m.version = 0x00010007;
  m.features = 0xF000000000000001;
  ready("r_minor", m);
  c.emplace_back("r_die",
                 Script().message(Op::kReady, good_ready()).exit(3));
  std::string failed_body;
  encode_failed_ready(wire::kVersion, "no heap", &failed_body);
  c.emplace_back("r_failed",
                 Script().message(Op::kReady, u(Status::kInternal), failed_body)
                     .exit(1));
  raw("r_failed_long", failed_body + "\xff\xff\xff", u(Status::kInternal));
  encode_failed_ready(0x00020000, "worker speaks 2.0, host 1.0", &failed_body);
  raw("r_failed_major", failed_body, u(Status::kInternal));
  raw("r_major2", std::string("\0\0\2\0", 4) + "\xff\xff what version 2 says");
  raw("r_major0", std::string("\3\0\0\0", 4) + good.substr(4));
  // Violations.
  raw("r_status", good, u(Status::kGuestError));
  raw("r_more", good, 0, kMore);
  raw("r_depth", good, 0, 0, mle_test::kAuto, mle_test::kAuto, 1);
  raw("r_seq", good, 0, 0, mle_test::kAuto, mle_test::kAuto, 0, 1);
  raw("r_reserved", good, 0, 0, mle_test::kAuto, mle_test::kAuto, 0, 0, 1);
  raw("r_big", good, 0, kMore, kArena, kArena + 1);
  raw("r_trailing", good + "x");
  raw("r_short", good.substr(0, good.size() - 1));
  raw("r_version_only", good.substr(0, 3));
  raw("r_failed_short", std::string("\0\0\1\0\x10\0", 6), u(Status::kInternal));
  encode_failed_ready(wire::kVersion, "\xff", &failed_body);
  raw("r_failed_text", failed_body, u(Status::kInternal));
  m = good_ready();
  m.worker_class = "Test";
  ready("r_class", m);
  m = good_ready();
  m.languages.clear();
  ready("r_no_language", m);
  m = good_ready();
  m.languages = {"none", "none"};
  ready("r_twice", m);
  m = good_ready();
  m.languages.clear();
  for (int i = 0; i < 17; i++) m.languages.push_back("l" + std::to_string(i));
  ready("r_17", m);
  c.emplace_back("r_other_op", Script().post(u(Op::kResult), 0, 0,
                                             bytes_of(ResultMsg())));
  c.emplace_back("r_never", Script().sleep(60000));
  return c;
}

/** One message in the log of the worker: see Script::log. */
struct Logged {
  wire::MessageHeader h{};
  std::string body;
};

// ---------------------------------------------------------------- fixture

enum class Where {
  /** At the CALL of a function without parameters that returns BIGINT. */
  kCall,
  /** At the CALL of a procedure without parameters. */
  kProcedure,
  /** At the CALL of a function with parameters a IN, b OUT, c INOUT. */
  kOuts,
  kPrepare,
  /** PREPARE with check. */
  kPrepareCheck,
  kParseLibrary,
  kDrop,
  kReset,
  kOptions,
  /** In place of the CONTINUE for the first chunk of a PREPARE. */
  kContinue,
};

class MleWorkerProtocol : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = kArena;
    auto add = [&c](const std::string &name, std::vector<std::string> args) {
      ClassConfig k;
      k.name = name;
      k.executable = MLE_TEST_WORKER_EXE;
      k.args = std::move(args);
      k.exec_paths = {dir_of(MLE_TEST_WORKER_EXE)};
      c.classes.push_back(k);
    };
    add("good", {"proto"});
    for (const auto &[name, script] : ready_classes())
      add(name, {"proto", script.hex()});
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
  }
  static void TearDownTestSuite() { delete s_launcher; }

  std::unique_ptr<Worker> acquire(const std::string &cls,
                                  WorkerOptions options = {}) {
    std::string err;
    auto p = s_launcher->acquire(cls, &err);
    EXPECT_NE(p, nullptr) << err;
    if (!p) return nullptr;
    return std::make_unique<Worker>(std::move(p), options);
  }

  /** A good worker that is ready and has options in force. */
  std::unique_ptr<Worker> start(WorkerOptions options = {}) {
    auto w = acquire("good", options);
    if (!w) return nullptr;
    ReadyMsg ready;
    std::string err;
    EXPECT_EQ(w->wait_ready(10000, &ready, &err), Outcome::kOk) << err;
    EXPECT_EQ(w->options(OptionsMsg(), nullptr, &err), Outcome::kOk) << err;
    return w;
  }

  static PrepareMsg routine(uint64_t id, std::string source,
                            bool returns = true) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = "none";
    p.name = "r" + std::to_string(id);
    p.source = std::move(source);
    p.schema = "test";
    if (returns) {
      ParamType t;
      t.tag = Tag::kI64;
      t.type_name = "BIGINT";
      p.returns = t;
    }
    return p;
  }

  /** Prepares routine id; its CALL runs the script. */
  void prepare(Worker &w, uint64_t id, const Script &at_call,
               bool returns = true) {
    ResultMsg res;
    std::string err;
    ASSERT_EQ(w.prepare(routine(id, "call:" + at_call.hex(), returns), hooks,
                        &res, &err),
              Outcome::kOk)
        << err;
  }

  struct Ran {
    std::unique_ptr<Worker> w;
    Outcome outcome = Outcome::kOk;
    std::string error;
    ResultMsg result;
    Error guest;
  };

  /** A fresh worker; the script runs at the place where says. */
  Ran run(Where where, const Script &script, WorkerOptions options = {}) {
    Ran r;
    r.w = start(options);
    if (!r.w) return r;
    Worker &w = *r.w;
    ResultMsg res;
    std::string err;
    auto armed = [&](Op op) {
      const Script arm = Script().arm(op, script).answer();
      EXPECT_EQ(
          w.prepare(routine(1, "prepare:" + arm.hex()), hooks, &res, &err),
          Outcome::kOk)
          << err;
    };
    switch (where) {
      case Where::kCall:
      case Where::kProcedure:
      case Where::kOuts: {
        PrepareMsg p =
            routine(1, "call:" + script.hex(), where != Where::kProcedure);
        std::vector<Value> args;
        if (where == Where::kOuts) {
          p.params = {{"a", ParamMode::kIn, *p.returns},
                      {"b", ParamMode::kOut, *p.returns},
                      {"c", ParamMode::kInOut, *p.returns}};
          args = {Value::i64(1), Value::null(), Value::i64(3)};
        }
        EXPECT_EQ(w.prepare(p, hooks, &res, &err), Outcome::kOk) << err;
        r.outcome = w.call(1, 1, args, hooks, &r.result, &r.error);
        break;
      }
      case Where::kPrepare:
      case Where::kPrepareCheck: {
        PrepareMsg p = routine(1, "prepare:" + script.hex());
        p.check = where == Where::kPrepareCheck;
        r.outcome = w.prepare(p, hooks, &r.result, &r.error);
        break;
      }
      case Where::kParseLibrary:
        r.outcome = w.parse_library({"none", "lib", "script:" + script.hex()},
                                    hooks, &r.result, &r.error);
        break;
      case Where::kDrop:
        armed(Op::kDrop);
        r.outcome = w.drop(1, &r.error);
        break;
      case Where::kReset:
        armed(Op::kReset);
        r.outcome = w.reset(&r.error);
        break;
      case Where::kOptions:
        // OPTIONS goes only to a worker with nothing prepared: after RESET.
        armed(Op::kOptions);
        EXPECT_EQ(w.reset(&err), Outcome::kOk) << err;
        r.outcome = w.options(OptionsMsg(), &r.guest, &r.error);
        break;
      case Where::kContinue:
        armed(Op::kContinue);
        r.outcome = w.prepare(routine(2, std::string(100000, 'x')), hooks,
                              &r.result, &r.error);
        break;
    }
    return r;
  }

  /** The script is a violation: the worker is ended, with that outcome. */
  void violation(const char *what, Where where, const Script &script,
                 const char *text) {
    SCOPED_TRACE(what);
    Ran r = run(where, script);
    ASSERT_NE(r.w, nullptr);
    EXPECT_EQ(r.outcome, Outcome::kViolation) << outcome_name(r.outcome);
    EXPECT_NE(r.error.find("broke the protocol"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find(text), std::string::npos) << r.error;
    EXPECT_FALSE(r.w->usable());
    EXPECT_EQ(r.w->failure(), r.error);
    EXPECT_TRUE(r.w->process().wait_exit(2000)) << "still running";
    EXPECT_EQ(r.w->process().exit_info(2000).describe(),
              "killed by signal 9 (Killed)");
    // Nothing more is sent to it.
    std::string err;
    EXPECT_EQ(r.w->reset(&err), Outcome::kDied);
  }

  /** The script is legal: the request ends as expected and the worker
      stays in use. */
  Ran legal(const char *what, Where where, const Script &script,
            Outcome expected) {
    SCOPED_TRACE(what);
    Ran r = run(where, script);
    if (!r.w) return r;
    EXPECT_EQ(r.outcome, expected) << outcome_name(r.outcome) << " " << r.error;
    EXPECT_TRUE(r.w->usable()) << r.w->failure();
    std::string err;
    EXPECT_EQ(r.w->drop(77, &err), Outcome::kOk) << err;
    return r;
  }

  static std::vector<Logged> parse_log(const std::string &log) {
    std::vector<Logged> out;
    size_t at = 0;
    while (log.size() - at >= sizeof(wire::MessageHeader)) {
      Logged l;
      memcpy(&l.h, log.data() + at, sizeof l.h);
      at += sizeof l.h;
      if (l.h.total_len > log.size() - at) break;
      l.body = log.substr(at, l.h.total_len);
      at += l.h.total_len;
      out.push_back(std::move(l));
    }
    EXPECT_EQ(at, log.size()) << "the log has a tail";
    return out;
  }

  /** The SQL_RESULTs in a log. */
  static std::vector<SqlResultMsg> replies(const std::string &log) {
    std::vector<SqlResultMsg> out;
    for (const Logged &l : parse_log(log)) {
      if (l.h.op != u(Op::kSqlResult)) continue;
      SqlResultMsg m;
      std::string err;
      EXPECT_TRUE(decode(l.body, &m, &err)) << err;
      out.push_back(std::move(m));
    }
    return out;
  }

  /** Sets the kill flag after a while, on another thread. */
  std::thread kill_after(int ms) {
    return std::thread([this, ms] {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      hooks.kill = true;
    });
  }

  static WorkerOptions grace() {
    WorkerOptions o;
    o.interrupt_grace_ms = kGrace;
    return o;
  }

  Hooks hooks;
  static Launcher *s_launcher;
};

Launcher *MleWorkerProtocol::s_launcher = nullptr;

// ---------------------------------------------------------------- basics

TEST_F(MleWorkerProtocol, GoodWorker) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  EXPECT_EQ(w->languages(), std::vector<std::string>{"none"});
  EXPECT_EQ(w->minor_version(), 0u);
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_TRUE(w->is_prepared(1));
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
    ASSERT_EQ(*res.ret, Value::i64(42));
  }
  EXPECT_EQ(w->parse_library({"none", "lib", "x"}, hooks, &res, &err),
            Outcome::kOk)
      << err;
  EXPECT_EQ(w->drop(1, &err), Outcome::kOk) << err;
  EXPECT_FALSE(w->is_prepared(1));
  EXPECT_EQ(w->drop(1, &err), Outcome::kOk) << err;  // not prepared: no error
  EXPECT_EQ(w->reset(&err), Outcome::kOk) << err;
  EXPECT_TRUE(w->usable());
  EXPECT_EQ(w->depth(), 0u);
}

// A request that waits long tells the hooks once per wait (a kill handler
// is set up only for it); a kill during the wait is still noticed.
TEST_F(MleWorkerProtocol, LongWaitsTellTheHooks) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  prepare(*w, 1, Script().sleep(50).answer());
  ResultMsg res;
  std::string err;
  hooks.sleeps = 0;
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(hooks.sleeps, 1);
  prepare(*w, 2, Script().sleep(5000).answer());
  std::thread killer = kill_after(30);
  const auto t0 = Clock::now();
  EXPECT_NE(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk);
  killer.join();
  EXPECT_TRUE(w->interrupted());
  EXPECT_LT(ms_since(t0), 4000);
}

// The host sends no language the worker did not announce (§1 item 4).
TEST_F(MleWorkerProtocol, LanguageNotInReady) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  PrepareMsg p = routine(1, "");
  p.language = "js";
  EXPECT_EQ(w->prepare(p, hooks, &res, &err), Outcome::kRefused);
  EXPECT_NE(err.find("language 'js'"), std::string::npos) << err;
  EXPECT_EQ(w->parse_library({"python", "lib", "x"}, hooks, &res, &err),
            Outcome::kRefused);
  EXPECT_TRUE(w->usable());
  EXPECT_FALSE(w->is_prepared(1));
}

// What the host does not send (§8, §9.8, §12): the worker is not involved.
TEST_F(MleWorkerProtocol, RequestsTheHostRefuses) {
  auto w = acquire("good");
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->reset(&err), Outcome::kRefused);  // not ready
  ReadyMsg ready;
  ASSERT_EQ(w->wait_ready(10000, &ready, &err), Outcome::kOk) << err;
  EXPECT_EQ(w->wait_ready(100, &ready, &err), Outcome::kRefused);
  // Without options in force: no PREPARE, CALL or PARSE_LIBRARY.
  EXPECT_FALSE(w->options_in_force());
  EXPECT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kRefused);
  EXPECT_NE(err.find("options"), std::string::npos) << err;
  EXPECT_EQ(w->parse_library({"none", "l", "x"}, hooks, &res, &err),
            Outcome::kRefused);
  EXPECT_EQ(w->drop(1, &err), Outcome::kOk) << err;  // DROP needs none
  // Options the host must not send.
  OptionsMsg bad;
  bad.settings = {{"b", ""}, {"a", ""}};
  EXPECT_EQ(w->options(bad, nullptr, &err), Outcome::kRefused);
  bad.settings = {{"a", std::string(1025, 'v')}};
  EXPECT_EQ(w->options(bad, nullptr, &err), Outcome::kRefused);
  bad = OptionsMsg();
  bad.time_zone.clear();
  EXPECT_EQ(w->options(bad, nullptr, &err), Outcome::kRefused);
  // The largest OPTIONS (256 settings of 64 + 1024 bytes) is over 64 KiB.
  OptionsMsg large;
  for (int i = 0; i < 100; i++)
    large.settings.push_back({"s" + std::to_string(100 + i),
                              std::string(1024, 'v')});
  EXPECT_EQ(w->options(large, nullptr, &err), Outcome::kRefused);
  EXPECT_NE(err.find("larger"), std::string::npos) << err;
  EXPECT_FALSE(w->options_in_force());
  OptionsMsg good;
  good.time_zone = "Europe/Kyiv";
  good.settings = {{"decimal_type", "number"}};
  ASSERT_EQ(w->options(good, nullptr, &err), Outcome::kOk) << err;
  EXPECT_TRUE(w->options_in_force());
  // A CALL of an id that is not prepared, or with other arguments.
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kRefused);
  ASSERT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(w->call(1, 0, {Value::i64(1)}, hooks, &res, &err),
            Outcome::kRefused);
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  // RESET ends the options.
  ASSERT_EQ(w->reset(&err), Outcome::kOk) << err;
  EXPECT_FALSE(w->options_in_force());
  EXPECT_FALSE(w->is_prepared(1));
  EXPECT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kRefused);
  EXPECT_TRUE(w->usable());
}

// §17: the CALL body and its header as the host posts them with call_seq 3,
// and the INTERRUPT datagram for call_seq 5.
TEST_F(MleWorkerProtocol, SpecVectorsOnTheWire) {
  auto w = start();  // OPTIONS: call_seq 1
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  PrepareMsg p = routine(7, "call:" + Script().log().answer().hex());
  ParamType vec;
  vec.tag = Tag::kVector;
  vec.length = 2;
  vec.type_name = "VECTOR";
  p.params = {{"a", ParamMode::kIn, *p.returns}, {"v", ParamMode::kIn, vec}};
  ASSERT_EQ(w->prepare(p, hooks, &res, &err), Outcome::kOk) << err;  // 2
  ASSERT_EQ(w->call(7, 1, {Value::i64(-2), Value::vector({1.0f, 2.0f})}, hooks,
                    &res, &err),
            Outcome::kOk)
      << err;  // 3
  const unsigned char header_and_body[] = {
      0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,     // op .. depth
      0x38, 0, 0, 0, 0, 0, 0, 0,                             // len, reserved
      0x38, 0, 0, 0, 0, 0, 0, 0,                             // total_len
      0x03, 0, 0, 0, 0, 0, 0, 0,                             // call_seq
      0x07, 0, 0, 0, 0, 0, 0, 0,                             // routine_id
      0x01, 0, 0, 0, 0, 0, 0, 0,                             // library_epoch
      0x02, 0, 0, 0,                                         // 2 arguments
      0x01, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // I64 -2
      0x0D, 0x02, 0, 0, 0,                                   // VECTOR, 2
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,              // padding to 48
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40};       // 1.0f, 2.0f
  EXPECT_EQ(res.console_out,
            std::string(reinterpret_cast<const char *>(header_and_body),
                        sizeof header_and_body));

  ASSERT_EQ(w->prepare(routine(8, "call:" + Script()
                                                .log()
                                                .await_interrupt()
                                                .answer()
                                                .hex()),
                       hooks, &res, &err),
            Outcome::kOk)
      << err;  // 4
  std::thread killer = kill_after(50);
  EXPECT_EQ(w->call(8, 1, {}, hooks, &res, &err), Outcome::kOk) << err;  // 5
  killer.join();
  EXPECT_TRUE(w->interrupted());
  ASSERT_GE(res.console_out.size(), 9u);
  EXPECT_EQ(res.console_out.substr(res.console_out.size() - 9),
            std::string("\x01\x05\0\0\0\0\0\0\0", 9));
  // The record is the request's: one that is not sent has none.
  EXPECT_EQ(w->call(99, 1, {}, hooks, &res, &err), Outcome::kRefused);
  EXPECT_FALSE(w->interrupted());
  EXPECT_TRUE(w->usable());
}

// §17, §5.2: a message of arena_size + 10 bytes, in both directions. The
// test worker ends itself (status 98) if a chunk or a CONTINUE of the host
// is not exactly as §5.2 says.
TEST_F(MleWorkerProtocol, ChunkedMessages) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  ResultMsg big;
  big.console_out.assign(kArena, 'c');
  big.console_out.resize(big.console_out.size() - bytes_of(big).size() +
                         kArena + 10);
  ASSERT_EQ(bytes_of(big).size(), kArena + 10u);
  PrepareMsg p = routine(1, "call:" + Script().result(big).hex(), false);
  ParamType blob;
  blob.tag = Tag::kBytes;
  blob.type_name = "BLOB";
  p.params = {{"a", ParamMode::kIn, blob}};
  ASSERT_EQ(w->prepare(p, hooks, &res, &err), Outcome::kOk) << err;
  // CALL: u64 id, u64 epoch, u32 count, then tag, u32 length and the bytes.
  const std::vector<Value> args = {
      Value::bytes(std::string(kArena + 10 - 25, 'a'))};
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(w->call(1, 0, args, hooks, &res, &err), Outcome::kOk) << err;
    EXPECT_EQ(res.console_out, big.console_out);
  }
  // Many chunks: 5 MiB there, 3 MiB back.
  big.console_out.assign(3u << 20, 'd');
  ASSERT_EQ(w->prepare(routine(2, "call:" + Script().result(big).hex(), false),
                       hooks, &res, &err),
            Outcome::kOk)
      << err;
  p.routine_id = 3;
  p.source = "call:" + Script().log().answer().hex();
  ASSERT_EQ(w->prepare(p, hooks, &res, &err), Outcome::kOk) << err;
  ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(res.console_out.size(), 3u << 20);
  const std::string blob5(5u << 20, 'e');
  ASSERT_EQ(w->call(3, 0, {Value::bytes(blob5)}, hooks, &res, &err),
            Outcome::kOk)
      << err;
  const auto log = parse_log(res.console_out);
  ASSERT_EQ(log.size(), 1u);
  CallMsg seen;
  ASSERT_TRUE(decode(log[0].body, &seen, &err)) << err;
  EXPECT_EQ(seen.args[0].as_text(), blob5);
  EXPECT_TRUE(w->usable());
}

// §8.2: a call_seq is used up when a request's first chunk is posted, and
// not by a request the host does not send.
TEST_F(MleWorkerProtocol, CallSequence) {
  auto w = start();  // 1
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().log().answer());  // 2
  auto seq_of_call = [&] {
    EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
    const auto log = parse_log(res.console_out);
    return log.size() == 1 ? log[0].h.call_seq : 0;
  };
  EXPECT_EQ(seq_of_call(), 3u);
  // Killed before the request starts: nothing is sent (§11.1).
  hooks.kill = true;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted);
  EXPECT_EQ(w->prepare(routine(5, ""), hooks, &res, &err),
            Outcome::kInterrupted);
  EXPECT_EQ(w->parse_library({"none", "l", "x"}, hooks, &res, &err),
            Outcome::kInterrupted);
  EXPECT_FALSE(w->is_prepared(5));
  EXPECT_FALSE(w->interrupted());
  hooks.kill = false;
  EXPECT_EQ(w->call(9, 0, {}, hooks, &res, &err), Outcome::kRefused);
  EXPECT_EQ(seq_of_call(), 4u);
  EXPECT_EQ(w->drop(9, &err), Outcome::kOk);  // 5
  EXPECT_EQ(seq_of_call(), 6u);
}

// ---------------------------------------------------------------- READY

TEST_F(MleWorkerProtocol, Ready) {
  auto outcome = [this](const char *cls, std::string *err,
                        std::string_view language = {}, int timeout = 10000,
                        std::unique_ptr<Worker> *keep = nullptr) {
    auto w = acquire(cls);
    if (!w) return Outcome::kRefused;
    ReadyMsg ready;
    const Outcome o = w->wait_ready(timeout, &ready, err, &hooks, language);
    EXPECT_EQ(w->usable(), o == Outcome::kOk) << cls;
    if (o != Outcome::kOk || std::string_view(cls) == "r_die") {
      EXPECT_TRUE(w->process().wait_exit(2000)) << cls;
      std::string again;
      EXPECT_EQ(w->reset(&again), Outcome::kDied) << cls;
    }
    if (keep != nullptr) *keep = std::move(w);
    return o;
  };
  std::string err;
  // A minor version and feature bits the host does not know are ignored.
  std::unique_ptr<Worker> w;
  EXPECT_EQ(outcome("r_minor", &err, {}, 10000, &w), Outcome::kOk) << err;
  ASSERT_NE(w, nullptr);
  EXPECT_EQ(w->minor_version(), 0u);
  EXPECT_EQ(w->options(OptionsMsg(), nullptr, &err), Outcome::kOk) << err;
  // The language that is needed must be listed.
  EXPECT_EQ(outcome("good", &err, "none"), Outcome::kOk) << err;
  EXPECT_EQ(outcome("good", &err, "js"), Outcome::kIncompatible);
  EXPECT_NE(err.find("'js'"), std::string::npos) << err;
  // A failed READY: the reason is reported; it may end after info.
  EXPECT_EQ(outcome("r_failed", &err), Outcome::kInternal);
  EXPECT_EQ(err, "the MLE worker failed to start: no heap");
  EXPECT_EQ(outcome("r_failed_long", &err), Outcome::kInternal);
  EXPECT_EQ(err, "the MLE worker failed to start: no heap");
  // Another major version is an incompatible worker, not a violation, and
  // nothing after the version is looked at.
  EXPECT_EQ(outcome("r_failed_major", &err), Outcome::kIncompatible);
  EXPECT_NE(err.find("worker speaks 2.0, host 1.0"), std::string::npos) << err;
  EXPECT_EQ(outcome("r_major2", &err), Outcome::kIncompatible);
  EXPECT_NE(err.find("version 2.0, the host 1.0"), std::string::npos) << err;
  EXPECT_EQ(outcome("r_major0", &err), Outcome::kIncompatible);
  EXPECT_NE(err.find("version 0.3"), std::string::npos) << err;
  // A worker that posts READY and ends: the READY is read (§11.4), and the
  // death is reported with it or by the first request, whichever the host
  // sees first.
  const Outcome last = outcome("r_die", &err);
  if (last != Outcome::kOk) {
    EXPECT_EQ(last, Outcome::kDied);
    EXPECT_EQ(err, "the MLE worker died: exited with status 3");
  }
  // The deadline, and a kill before READY.
  const auto t0 = Clock::now();
  EXPECT_EQ(outcome("r_never", &err, {}, 200), Outcome::kTimedOut);
  EXPECT_NE(err.find("no READY"), std::string::npos) << err;
  EXPECT_GE(ms_since(t0), 200);
  EXPECT_LT(ms_since(t0), 2000);
  hooks.kill = true;
  EXPECT_EQ(outcome("r_never", &err), Outcome::kInterrupted);
  hooks.kill = false;
}

TEST_F(MleWorkerProtocol, ReadyViolations) {
  const std::vector<std::pair<const char *, const char *>> cases = {
      {"r_status", "bad status"},
      {"r_more", "does not fill the arena"},
      {"r_depth", "wrong request"},
      {"r_seq", "wrong request"},
      {"r_reserved", "reserved"},
      {"r_big", "READY larger than its limit"},
      {"r_trailing", "bad READY: trailing bytes"},
      {"r_short", "bad READY"},
      {"r_version_only", "bad READY"},
      {"r_failed_short", "bad READY"},
      {"r_failed_text", "bad READY: invalid UTF-8"},
      {"r_class", "bad worker class"},
      {"r_no_language", "no languages"},
      {"r_twice", "listed twice"},
      {"r_17", "too many elements"},
      {"r_other_op", "unexpected RESULT during READY"},
  };
  for (const auto &[cls, text] : cases) {
    SCOPED_TRACE(cls);
    auto w = acquire(cls);
    ASSERT_NE(w, nullptr);
    ReadyMsg ready;
    std::string err;
    EXPECT_EQ(w->wait_ready(10000, &ready, &err), Outcome::kViolation) << err;
    EXPECT_NE(err.find("broke the protocol"), std::string::npos) << err;
    EXPECT_NE(err.find(text), std::string::npos) << err;
    EXPECT_FALSE(w->usable());
    EXPECT_TRUE(w->process().wait_exit(2000));
  }
}

// ---------------------------------------------------------------- violations

// §13: msg.op, msg.status, msg.flags, msg.depth and msg.call_seq, msg.len
// and msg.total_len, msg reserved.
TEST_F(MleWorkerProtocol, HeaderViolations) {
  const std::string good = bytes_of(good_result());
  const std::string plain = bytes_of(ResultMsg());
  const std::string full(kArena, 'f');
  auto op = [&](uint32_t o, std::string_view arena = {}) {
    return Script().post(o, 0, 0, arena);
  };
  // op: assigned, and allowed here.
  violation("op 99", Where::kCall, op(99, good), "unknown op");
  violation("op 0", Where::kCall, op(0, good), "unknown op");
  violation("op 7", Where::kCall, op(7, good), "unknown op");
  violation("PREPARE of a worker", Where::kCall, op(u(Op::kPrepare)),
            "unexpected PREPARE during CALL");
  violation("SQL_RESULT of a worker", Where::kCall, op(u(Op::kSqlResult)),
            "unexpected SQL_RESULT during CALL");
  violation("LIBRARY_RESULT of a worker", Where::kPrepare,
            op(u(Op::kLibraryResult)),
            "unexpected LIBRARY_RESULT during PREPARE");
  violation("READY again", Where::kCall, op(u(Op::kReady), bytes_of(good_ready())),
            "unexpected READY during CALL");
  violation("CONTINUE out of place", Where::kCall, op(u(Op::kContinue)),
            "unexpected CONTINUE during CALL");
  const std::string select = bytes_of(execute());
  const std::string import = bytes_of(library());
  violation("SQL in PARSE_LIBRARY", Where::kParseLibrary, op(u(Op::kSql), select),
            "unexpected SQL during PARSE_LIBRARY");
  violation("LIBRARY in PARSE_LIBRARY", Where::kParseLibrary,
            op(u(Op::kLibrary), import),
            "unexpected LIBRARY during PARSE_LIBRARY");
  violation("SQL in DROP", Where::kDrop, op(u(Op::kSql), select),
            "unexpected SQL during DROP");
  violation("SQL in RESET", Where::kReset, op(u(Op::kSql), select),
            "unexpected SQL during RESET");
  violation("LIBRARY in OPTIONS", Where::kOptions, op(u(Op::kLibrary), import),
            "unexpected LIBRARY during OPTIONS");
  // status: 0 unless RESULT.
  violation("status 5", Where::kCall, raw_result(good, 5), "bad status");
  violation("a host outcome as status", Where::kCall, raw_result(good, 7),
            "bad status");
  violation("status of an SQL", Where::kCall,
            Script().post(u(Op::kSql), 1, 0, select), "bad status");
  violation("status of a LIBRARY", Where::kCall,
            Script().post(u(Op::kLibrary), 4, 0, import), "bad status");
  // flags: only MORE and TAINTED, and MORE as §5.2 says.
  violation("unknown flag", Where::kCall, raw_result(good, 0, 4),
            "unknown flags");
  violation("unknown flag beside TAINTED", Where::kCall,
            raw_result(good, 0, wire::kTainted | 8), "unknown flags");
  violation("MORE on a short chunk", Where::kCall,
            raw_result(good, 0, kMore, mle_test::kAuto, 2 * kArena),
            "does not fill the arena");
  violation("MORE on a whole message", Where::kCall,
            raw_result(full, 0, kMore, kArena, kArena),
            "more chunks announced after the last byte");
  // depth and call_seq: on the first chunk and on every other.
  violation("depth", Where::kCall, raw_result(good, 0, 0, mle_test::kAuto,
                                               mle_test::kAuto, 1),
            "RESULT for the wrong request");
  violation("call_seq", Where::kCall,
            raw_result(good, 0, 0, mle_test::kAuto, mle_test::kAuto, 0, 1),
            "RESULT for the wrong request");
  violation("call_seq of an earlier request", Where::kCall,
            raw_result(good, 0, 0, mle_test::kAuto, mle_test::kAuto, 0, -1),
            "RESULT for the wrong request");
  violation("depth of an SQL", Where::kCall,
            Script().post(u(Op::kSql), 0, 0, select, mle_test::kAuto,
                          mle_test::kAuto, 1),
            "SQL for the wrong request");
  violation("depth on a first chunk", Where::kCall,
            raw_result(full, 0, kMore, kArena, 2 * kArena, 1),
            "RESULT for the wrong request");
  auto second = [&](Script chunk) {
    Script s = raw_result(full, 0, kMore, kArena, 2 * kArena);
    return Script(s).wait().loop(1, chunk);
  };
  violation("depth on a later chunk", Where::kCall,
            second(raw_result(full, 0, 0, kArena, 2 * kArena, 1)),
            "does not continue the message");
  violation("call_seq on a later chunk", Where::kCall,
            second(raw_result(full, 0, 0, kArena, 2 * kArena, 0, 1)),
            "does not continue the message");
  violation("total_len on a later chunk", Where::kCall,
            second(raw_result(full, 0, 0, kArena, 2 * kArena + 1)),
            "does not continue the message");
  violation("op on a later chunk", Where::kCall,
            second(Script().post(u(Op::kSql), 0, 0, full, kArena, 2 * kArena)),
            "does not continue the message");
  violation("status on a later chunk", Where::kCall,
            second(raw_result(full, 1, 0, kArena, 2 * kArena)),
            "does not continue the message");
  violation("CONTINUE instead of a chunk", Where::kCall,
            second(Script().post(u(Op::kContinue), 0, 0, {}, 0, 0)),
            "does not continue the message");
  // len and total_len.
  violation("len over the arena", Where::kCall,
            raw_result(good, 0, 0, kArena + 1, kArena + 1),
            "larger than the arena");
  violation("len over total_len", Where::kCall, raw_result(good, 0, 0, 28, 27),
            "chunk larger than the message");
  violation("len under total_len", Where::kCall, raw_result(good, 0, 0, 28, 29),
            "message truncated");
  violation("a later chunk beyond the message", Where::kCall,
            second(raw_result(full, 0, 0, kArena, 2 * kArena - 1)),
            "does not continue");
  violation("more than 64 MiB", Where::kCall,
            raw_result(full, 0, kMore, kArena, wire::kMaxWorkerMessage + 1),
            "message too large");
  for (const Where where : {Where::kDrop, Where::kReset, Where::kOptions})
    violation("a RESULT of more than 64 KiB", where,
              raw_result(full, 0, kMore, kArena, kArena + 1),
              "RESULT larger than its limit");
  violation("a LIBRARY of more than 64 KiB", Where::kCall,
            Script().post(u(Op::kLibrary), 0, kMore, full, kArena, kArena + 1),
            "LIBRARY larger than its limit");
  // reserved.
  violation("reserved", Where::kCall,
            raw_result(good, 0, 0, mle_test::kAuto, mle_test::kAuto, 0, 0, 1),
            "reserved");
  violation("reserved of an SQL", Where::kCall,
            Script().post(u(Op::kSql), 0, 0, select, mle_test::kAuto,
                          mle_test::kAuto, 0, 0, 0x80000000),
            "reserved");
}

// §13, CONTINUE: every field as §5.2.
TEST_F(MleWorkerProtocol, ContinueViolations) {
  auto go = [](uint32_t status, uint32_t flags, uint32_t len, uint64_t total,
               int32_t depth = 0, int64_t seq = 0, uint32_t reserved = 0) {
    return Script().post(u(Op::kContinue), status, flags, {}, len, total, depth,
                         seq, reserved);
  };
  violation("len", Where::kContinue, go(0, 0, 1, 0), "bad CONTINUE");
  violation("total_len", Where::kContinue, go(0, 0, 0, 1), "bad CONTINUE");
  violation("status", Where::kContinue, go(1, 0, 0, 0), "bad status");
  violation("flags", Where::kContinue, go(0, kMore, 0, 0),
            "does not fill the arena");
  violation("depth", Where::kContinue, go(0, 0, 0, 0, 1), "expected CONTINUE");
  violation("call_seq", Where::kContinue, go(0, 0, 0, 0, 0, 1),
            "expected CONTINUE");
  violation("reserved", Where::kContinue, go(0, 0, 0, 0, 0, 0, 1), "reserved");
  violation("another op", Where::kContinue,
            Script().post(u(Op::kResult), 0, 0, bytes_of(ResultMsg())),
            "expected CONTINUE for a chunk of PREPARE");
  // A conforming CONTINUE in that place is taken.
  legal("a good CONTINUE", Where::kContinue, go(0, 0, 0, 0), Outcome::kOk);
}

// §13: the body is consumed exactly; counts, bools, tags; str and lstr;
// the domains of DECIMAL, Civil, TIMESTAMP and VECTOR.
TEST_F(MleWorkerProtocol, BodyViolations) {
  const std::string good = bytes_of(good_result());
  auto body = [](std::string bytes, uint32_t status = 0) {
    return Script().message(Op::kResult, status, bytes);
  };
  violation("trailing byte", Where::kCall, body(good + '\0'),
            "bad RESULT: trailing bytes");
  violation("missing byte", Where::kCall, body(good.substr(0, good.size() - 1)),
            "bad RESULT: message truncated");
  violation("empty body", Where::kCall, body(""), "bad RESULT");
  std::string bytes = good;
  bytes[0] = 2;
  violation("bool", Where::kCall, body(bytes), "bad boolean");
  bytes = good;
  bytes[1] = 14;
  violation("value tag", Where::kCall, body(bytes), "unknown value tag");
  bytes = std::string("\0\xff\xff\xff\xff", 5);
  violation("count beyond the bytes", Where::kCall, body(bytes),
            "element count beyond the message");
  bytes = bytes_of(ResultMsg());
  bytes.replace(1, 4, std::string("\0\0\1\0", 4));  // 65536 OUT parameters
  bytes.insert(5, std::string(65536 * 5, '\0'));
  violation("more values than a list may have", Where::kProcedure, body(bytes),
            "too many elements");
  // Text.
  violation("STRING that is not UTF-8", Where::kCall,
            body(bytes_of(returning(Value::string("caf\xe9")))),
            "invalid UTF-8");
  violation("JSON that is not UTF-8", Where::kCall,
            body(bytes_of(returning(Value::json("\"\xed\xa0\x80\"")))),
            "invalid UTF-8");
  violation("message that is not UTF-8", Where::kCall,
            body(bytes_of(failed("", "\xc0\xaf")), u(Status::kGuestError)),
            "invalid UTF-8");
  // Domains.
  Temporal t;
  t.year = 2026, t.month = 13, t.day = 1;
  violation("DATE", Where::kCall,
            body(bytes_of(returning(Value::temporal(Tag::kDate, t)))),
            "out of range DATE");
  t = Temporal();
  t.hour = 839;
  violation("TIME", Where::kCall,
            body(bytes_of(returning(Value::temporal(Tag::kTime, t)))),
            "out of range TIME");
  t = Temporal();
  t.year = 2026, t.month = 1, t.day = 1, t.hour = 24;
  violation("DATETIME", Where::kCall,
            body(bytes_of(returning(Value::temporal(Tag::kDatetime, t)))),
            "out of range DATETIME");
  violation("TIMESTAMP", Where::kCall,
            body(bytes_of(returning(Value::timestamp(-1)))),
            "out of range TIMESTAMP");
  violation("DECIMAL", Where::kCall,
            body(bytes_of(returning(Value::decimal("1e5")))), "bad DECIMAL");
  violation("DECIMAL of 101 bytes", Where::kCall,
            body(bytes_of(returning(Value::decimal(std::string(101, '1'))))),
            "bad DECIMAL");
  violation("VECTOR without dimensions", Where::kCall,
            body(bytes_of(returning(Value::vector({})))),
            "bad VECTOR dimensions");
  violation("VECTOR of 16384 dimensions", Where::kCall,
            body(bytes_of(returning(
                Value::vector(std::vector<float>(16384, 1.0f))))),
            "bad VECTOR dimensions");
  bytes = bytes_of(returning(Value::vector({1.0f})));
  bytes[7] = 1;  // has_return, tag, dims, then the padding
  violation("VECTOR padding", Where::kCall, body(bytes),
            "non-zero VECTOR padding");
  // A value in an OUT parameter is checked like any other.
  ResultMsg outs = good_result();
  outs.out_params = {{1, Value::i64(1)}, {2, Value::decimal("-")}};
  violation("DECIMAL in an OUT parameter", Where::kOuts, body(bytes_of(outs)),
            "bad DECIMAL");
}

// §13, RESULT: every rule of §9.4 and of the request table of §9.
TEST_F(MleWorkerProtocol, ResultViolations) {
  auto result = [](const ResultMsg &m, Status status = Status::kOk) {
    return Script().result(m, u(status));
  };
  const char *kStatus = "a status that is not allowed for the request";
  // Statuses per request.
  for (const Where where : {Where::kDrop, Where::kReset})
    for (const Status s :
         {Status::kGuestError, Status::kInterrupted, Status::kExhausted})
      violation("status of DROP or RESET", where, fail_with(s), kStatus);
  for (const Status s : {Status::kInterrupted, Status::kExhausted})
    violation("status of OPTIONS", Where::kOptions, fail_with(s), kStatus);
  for (const Where where :
       {Where::kCall, Where::kPrepare, Where::kParseLibrary})
    violation("INTERRUPTED without a record", where,
              fail_with(Status::kInterrupted), "INTERRUPTED without");
  // has_error exactly when the status is not OK.
  for (const Status s : {Status::kGuestError, Status::kInterrupted,
                         Status::kExhausted, Status::kInternal})
    violation("status without an Error", Where::kCall, result(ResultMsg(), s),
              "status and error disagree");
  ResultMsg m = good_result();
  m.error = Error{"none", "boom", {}, 0, "", 0};
  violation("OK with an Error", Where::kCall, result(m), "disagree");
  // has_return.
  violation("no return value", Where::kCall, result(ResultMsg()),
            "missing return value");
  violation("return value of a procedure", Where::kProcedure,
            result(good_result()), "unexpected return value");
  m = failed("none");
  m.ret = Value::i64(1);
  violation("return value with an error", Where::kCall,
            result(m, Status::kGuestError), "unexpected return value");
  for (const Where where : {Where::kPrepare, Where::kParseLibrary, Where::kDrop,
                            Where::kReset, Where::kOptions})
    violation("return value of another request", where, result(good_result()),
              "unexpected return value");
  // out_params: one for every OUT and INOUT parameter, ascending.
  auto outs = [&](std::vector<uint32_t> indexes) {
    ResultMsg r = good_result();
    for (const uint32_t i : indexes) r.out_params.emplace_back(i, Value::i64(i));
    return result(r);
  };
  violation("no OUT values", Where::kOuts, outs({}), "bad OUT parameters");
  violation("an OUT value missing", Where::kOuts, outs({1}), "bad OUT");
  violation("another missing", Where::kOuts, outs({2}), "bad OUT");
  violation("descending", Where::kOuts, outs({2, 1}), "bad OUT");
  violation("twice", Where::kOuts, outs({1, 1, 2}), "bad OUT");
  violation("an IN parameter", Where::kOuts, outs({0, 1, 2}), "bad OUT");
  violation("no such parameter", Where::kOuts, outs({1, 2, 3}), "bad OUT");
  violation("counted from 1", Where::kOuts, outs({2, 3}), "bad OUT");
  violation("OUT value without parameters", Where::kCall, outs({0}), "bad OUT");
  m = failed("none");
  m.out_params = {{1, Value::i64(1)}, {2, Value::i64(2)}};
  violation("OUT values with an error", Where::kOuts,
            result(m, Status::kGuestError), "unexpected OUT parameters");
  m = ResultMsg();
  m.out_params = {{0, Value::null()}};
  violation("OUT values of a PREPARE", Where::kPrepare, result(m),
            "unexpected OUT parameters");
  // Console output and findings only where the table allows them.
  for (const Where where : {Where::kParseLibrary, Where::kDrop, Where::kReset,
                            Where::kOptions}) {
    m = ResultMsg();
    m.console_out = "x";
    violation("stdout", where, result(m), "console output where none");
    m = ResultMsg();
    m.console_err = "x";
    violation("stderr", where, result(m), "console output where none");
    m = ResultMsg();
    m.console_cleared = true;
    violation("console_cleared", where, result(m), "console output where none");
    m = ResultMsg();
    m.findings = {"x"};
    violation("findings", where, result(m), "findings where none");
  }
  m = good_result();
  m.findings = {"x"};
  violation("findings of a CALL", Where::kCall, result(m), "findings where");
  m = ResultMsg();
  m.findings = {"x"};
  violation("findings without check", Where::kPrepare, result(m),
            "findings where");
  // Findings go only with status OK (§9.4).
  m = failed("none");
  m.findings = {"x"};
  violation("findings of a failed PREPARE", Where::kPrepareCheck,
            result(m, Status::kGuestError), "findings where");
  // Error.language.
  const char *kLanguage = "an Error.language that is not allowed";
  violation("a language the worker did not list", Where::kCall,
            fail_with(Status::kGuestError, "js"), kLanguage);
  violation("MLE-Type of a PREPARE with another status", Where::kPrepare,
            fail_with(Status::kInternal, wire::kLanguageType), kLanguage);
  violation("MLE-Type of a PARSE_LIBRARY", Where::kParseLibrary,
            fail_with(Status::kGuestError, wire::kLanguageType), kLanguage);
  violation("MLE-Type with another status", Where::kCall,
            fail_with(Status::kExhausted, wire::kLanguageType), kLanguage);
  violation("MLE-TimeZone of a CALL", Where::kCall,
            fail_with(Status::kGuestError, wire::kLanguageTimeZone), kLanguage);
  violation("MLE-Option of a PREPARE", Where::kPrepare,
            fail_with(Status::kGuestError, wire::kLanguageOption), kLanguage);
  violation("a language for OPTIONS", Where::kOptions,
            fail_with(Status::kGuestError, "none"), kLanguage);
  violation("no language for OPTIONS", Where::kOptions,
            fail_with(Status::kGuestError, ""), kLanguage);
  violation("MLE-Type for OPTIONS", Where::kOptions,
            fail_with(Status::kGuestError, wire::kLanguageType), kLanguage);
  violation("a marker with INTERNAL", Where::kOptions,
            fail_with(Status::kInternal, wire::kLanguageOption), kLanguage);
  violation("a marker in other letters", Where::kCall,
            fail_with(Status::kGuestError, "mle-type"), kLanguage);
  // sql_errno and sqlstate, with every status.
  for (const Status s : {Status::kGuestError, Status::kInternal}) {
    m = failed("none");
    m.error->sql_errno = 65536;
    violation("sql_errno", Where::kCall, result(m, s), "bad SQL error number");
    m = failed("none");
    m.error->sqlstate = "42s02";
    violation("sqlstate", Where::kCall, result(m, s), "bad SQLSTATE");
    m.error->sqlstate = "4200";
    violation("sqlstate of 4", Where::kCall, result(m, s), "bad SQLSTATE");
  }
  // The caps of §7.4 and §9.4.
  const char *kLong = "string longer than its limit";
  m = failed("none", std::string(16385, 'm'));
  violation("message", Where::kCall, result(m, Status::kGuestError), kLong);
  m = failed("none");
  m.error->stack = {{std::string(1025, 'f'), "", 0, 0, 0, ""}};
  violation("function", Where::kCall, result(m, Status::kGuestError), kLong);
  m.error->stack = {{"", std::string(1025, 'f'), 0, 0, 0, ""}};
  violation("file", Where::kCall, result(m, Status::kGuestError), kLong);
  m.error->stack = {{"", "", 0, 0, 0, std::string(1025, 't')}};
  violation("text", Where::kCall, result(m, Status::kGuestError), kLong);
  m.error->stack.assign(1001, StackFrame());
  violation("frames", Where::kCall, result(m, Status::kGuestError),
            "too many elements");
  m = ResultMsg();
  m.findings = {std::string(4097, 'f')};
  violation("a finding", Where::kPrepareCheck, result(m), kLong);
  m.findings.assign(1001, "f");
  violation("findings", Where::kPrepareCheck, result(m), "too many elements");
}

// §13: SQL (action, flags, unused fields; the discarded message's head)
// and LIBRARY (schema, name).
TEST_F(MleWorkerProtocol, SqlAndLibraryViolations) {
  auto raw = [](std::string bytes, Op op = Op::kSql) {
    return Script().message(op, 0, bytes);
  };
  std::string bytes = bytes_of(execute());
  bytes[0] = 7;
  violation("action", Where::kCall, raw(bytes), "bad SQL action");
  bytes = bytes_of(execute());
  bytes[bytes.size() - 9] = 2;
  violation("flags", Where::kCall, raw(bytes), "unknown SQL flags");
  SqlMsg m = action(SqlAction::kFetch);
  m.statement = "SELECT 1";
  violation("text of a FETCH", Where::kCall, sql(m), "does not use");
  m = action(SqlAction::kPrepare);
  m.statement = "SELECT ?";
  m.params = {Value::i64(1)};
  violation("params of a PREPARE", Where::kCall, sql(m), "does not use");
  m = execute();
  m.row_width = 1;
  violation("row_width of an EXECUTE", Where::kCall, sql(m), "does not use");
  m = action(SqlAction::kExecutePrepared);
  m.flags = wire::kPassResultToClient;
  violation("flags of an EXECUTE_PREPARED", Where::kCall, sql(m),
            "does not use");
  m = action(SqlAction::kExecuteMany);
  m.max_rows = 5;
  violation("max_rows of an EXECUTE_MANY", Where::kCall, sql(m),
            "does not use");
  violation("text that is not UTF-8", Where::kCall, sql(execute("SELECT '\xff'")),
            "bad SQL: invalid UTF-8");
  m = execute("SELECT ?");
  m.params = {Value::decimal("")};
  violation("a parameter's domain", Where::kCall, sql(m), "bad DECIMAL");
  m.params = {Value::string("\xff")};
  violation("a parameter's text", Where::kCall, sql(m), "invalid UTF-8");
  violation("trailing byte", Where::kCall, raw(bytes_of(execute()) + '\0'),
            "bad SQL: trailing bytes");
  violation("shorter than action and id", Where::kCall, raw("\0\1\2\3"),
            "bad SQL: message truncated");
  bytes = bytes_of(action(SqlAction::kClose));
  violation("a CLOSE of 27 bytes", Where::kCall, raw(bytes + '\0'),
            "a CLOSE of the wrong length");
  // The same in a PREPARE, where SQL is refused but not unchecked.
  violation("unused field while serving PREPARE", Where::kPrepare,
            sql([] {
              SqlMsg f = action(SqlAction::kFetch);
              f.row_width = 1;
              return f;
            }()),
            "does not use");
  // Over the bound on request bytes only action and id are checked: they
  // still are.
  hooks.bytes_bound = 100;
  bytes = std::string(1000, '\xff');
  bytes[0] = 9;
  violation("action of a discarded message", Where::kCall, raw(bytes),
            "bad SQL action");
  bytes[0] = 5;
  violation("a long CLOSE", Where::kCall, raw(bytes), "a CLOSE of the wrong");
  hooks.bytes_bound = std::numeric_limits<uint64_t>::max();

  violation("empty schema", Where::kCall, Script().message(Op::kLibrary, library("")),
            "bad library name");
  violation("empty name", Where::kPrepare,
            Script().message(Op::kLibrary, library("db", "")),
            "bad library name");
  violation("name of 257 bytes", Where::kCall,
            Script().message(Op::kLibrary, library("db", std::string(257, 'n'))),
            "bad library name");
  violation("U+0000 in a name", Where::kCall,
            Script().message(Op::kLibrary,
                             library("db", std::string("a\0b", 3))),
            "bad library name");
  violation("a name that is not UTF-8", Where::kCall,
            Script().message(Op::kLibrary, library("db", "\xff")),
            "bad LIBRARY: invalid UTF-8");
  violation("trailing byte in a LIBRARY", Where::kCall,
            raw(bytes_of(library()) + '\0', Op::kLibrary),
            "bad LIBRARY: trailing bytes");
}

// ---------------------------------------------------------------- results

// What §9.4 allows, at the edges of the rules above.
TEST_F(MleWorkerProtocol, LegalResults) {
  auto result = [](const ResultMsg &m, Status status = Status::kOk) {
    return Script().result(m, u(status));
  };
  // Every status with its Error; the worker stays in use.
  for (const Where where :
       {Where::kCall, Where::kPrepare, Where::kParseLibrary})
    for (const Status s :
         {Status::kGuestError, Status::kExhausted, Status::kInternal})
      for (const char *language : {"", "none"}) {
        Ran r = legal("a failed request", where, fail_with(s, language),
                      static_cast<Outcome>(s));
        EXPECT_EQ(r.error, "boom");
        ASSERT_TRUE(r.result.error.has_value());
        EXPECT_EQ(r.result.error->language, language);
      }
  // MLE-Type is for a CALL's GUEST_ERROR.
  Ran r = legal("MLE-Type", Where::kCall,
                fail_with(Status::kGuestError, wire::kLanguageType),
                Outcome::kGuestError);
  EXPECT_EQ(r.result.error->language, "MLE-Type");

  // OUT parameters, console output with any bytes, findings with check.
  ResultMsg m = good_result();
  m.out_params = {{1, Value::string("out")}, {2, Value::null()}};
  m.console_out = std::string("\xff\0 any bytes", 12);
  m.console_err = "warning\n";
  m.console_cleared = true;
  r = legal("OUT values and console", Where::kOuts, result(m), Outcome::kOk);
  EXPECT_EQ(r.result, m);
  m = ResultMsg();
  m.findings.assign(1000, std::string(4096, 'f'));
  m.console_out = "printed while loading";
  r = legal("findings", Where::kPrepareCheck, result(m), Outcome::kOk);
  EXPECT_EQ(r.result, m);
  // An Error at its caps, with an SQL error and the id of one handed back.
  m = failed("none", std::string(16384, 'm'));
  m.error->stack.assign(1000, {std::string(1024, 'f'), std::string(1024, 'p'),
                               1, 2, 3, std::string(1024, 't')});
  m.error->sql_errno = 65535;
  m.error->sqlstate = "45000";
  m.error->sql_error_id = 0xfffffffffffffff0;
  m.console_err = "traceback";
  r = legal("the largest Error", Where::kCall, result(m, Status::kGuestError),
            Outcome::kGuestError);
  EXPECT_EQ(r.result, m);
  EXPECT_EQ(r.w->held_error(m.error->sql_error_id), nullptr);
  // Values of any tag, at the edges of their domains.
  for (const Value &v :
       {Value::null(), Value::timestamp(wire::kZeroTimestamp),
        Value::timestamp(wire::kMaxTimestamp), Value::decimal("-0.0"),
        Value::vector(std::vector<float>(16383, -0.0f)), Value::f64(-0.0),
        Value::json("{not json"), Value::bytes("\xff"),
        Value::string(std::string("\0", 1))}) {
    r = legal(tag_name(v.tag()), Where::kCall, result(returning(v)),
              Outcome::kOk);
    EXPECT_EQ(*r.result.ret, v);
  }
}

// §7.2, §9.4: a float of a worker that is not finite is an error of the
// request, as if the worker had answered GUEST_ERROR / MLE-Type; the worker
// lives on.
TEST_F(MleWorkerProtocol, NonFiniteResultsAreErrors) {
  const double inf = std::numeric_limits<double>::infinity();
  const float nanf = std::numeric_limits<float>::quiet_NaN();
  for (const Value &v : {Value::f64(inf), Value::f64(std::nan("")),
                         Value::f32(nanf), Value::vector({1.0f, nanf, 2.0f})}) {
    ResultMsg m = returning(v);
    m.console_out = "printed";
    Ran r = legal(tag_name(v.tag()), Where::kCall, Script().result(m),
                  Outcome::kGuestError);
    ASSERT_TRUE(r.result.error.has_value());
    EXPECT_EQ(r.result.error->language, wire::kLanguageType);
    EXPECT_FALSE(r.result.ret.has_value());
    EXPECT_EQ(r.result.console_out, "printed");
    EXPECT_NE(r.error.find("not finite"), std::string::npos) << r.error;
  }
  ResultMsg m = good_result();
  m.out_params = {{1, Value::i64(1)}, {2, Value::f64(-inf)}};
  Ran r = legal("OUT", Where::kOuts, Script().result(m), Outcome::kGuestError);
  EXPECT_EQ(r.result.error->language, wire::kLanguageType);
  EXPECT_TRUE(r.result.out_params.empty());
}

// §8.3: a PREPARE that does not answer OK, and a DROP that answers
// INTERNAL, leave the id not prepared.
TEST_F(MleWorkerProtocol, FailedPrepareLeavesTheIdUnprepared) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kOk) << err;
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  const Script fail = fail_with(Status::kGuestError, "none");
  EXPECT_EQ(w->prepare(routine(1, "prepare:" + fail.hex()), hooks, &res, &err),
            Outcome::kGuestError);
  EXPECT_FALSE(w->is_prepared(1));
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kRefused);
  EXPECT_TRUE(w->usable());
  // PREPARE of a prepared id replaces it: the RESULT of a CALL is checked
  // against the new parameters.
  ASSERT_EQ(w->prepare(routine(1, ""), hooks, &res, &err), Outcome::kOk) << err;
  ASSERT_EQ(w->prepare(routine(1, "", false), hooks, &res, &err), Outcome::kOk);
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_FALSE(res.ret.has_value());
}

// §9.8: the caller decides by Error.language; the options in force are
// unchanged by a refused OPTIONS.
TEST_F(MleWorkerProtocol, OptionsErrors) {
  for (const char *marker : {wire::kLanguageTimeZone, wire::kLanguageOption}) {
    Ran r = legal(marker, Where::kOptions,
                  fail_with(Status::kGuestError, marker), Outcome::kGuestError);
    EXPECT_EQ(r.guest.language, marker);
    EXPECT_EQ(r.guest.message, "boom");
    // As before the OPTIONS: none in force (it came after a RESET), and an
    // OPTIONS that failed puts none in force.
    EXPECT_FALSE(r.w->options_in_force());
    ResultMsg res;
    std::string err;
    EXPECT_EQ(r.w->prepare(routine(3, ""), hooks, &res, &err),
              Outcome::kRefused);
    EXPECT_EQ(r.w->options(OptionsMsg(), nullptr, &err), Outcome::kOk) << err;
    EXPECT_EQ(r.w->prepare(routine(3, ""), hooks, &res, &err), Outcome::kOk)
        << err;
  }
}

// §9.4, §11.5: INTERNAL from DROP, RESET or OPTIONS retires the worker; from
// PREPARE, CALL or PARSE_LIBRARY it does not (LegalResults). A tainted
// worker is retired after the top-level RESULT; the request keeps its
// outcome.
TEST_F(MleWorkerProtocol, Retirement) {
  for (const Where where : {Where::kDrop, Where::kReset, Where::kOptions}) {
    Ran r = run(where, fail_with(Status::kInternal));
    ASSERT_NE(r.w, nullptr);
    EXPECT_EQ(r.outcome, Outcome::kInternal);
    EXPECT_EQ(r.error, "boom");
    EXPECT_FALSE(r.w->usable());
    EXPECT_NE(r.w->failure().find("answered INTERNAL"), std::string::npos)
        << r.w->failure();
    EXPECT_TRUE(r.w->process().wait_exit(2000));
    std::string err;
    EXPECT_EQ(r.w->reset(&err), Outcome::kDied);
    EXPECT_EQ(err, r.w->failure());
  }
  Ran r = run(Where::kCall, Script().tainted().answer());
  ASSERT_NE(r.w, nullptr);
  EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
  EXPECT_EQ(*r.result.ret, Value::i64(42));
  EXPECT_FALSE(r.w->usable());
  EXPECT_NE(r.w->failure().find("retired"), std::string::npos);
  EXPECT_TRUE(r.w->process().wait_exit(2000));
  r = run(Where::kCall, Script().tainted().result(failed("none"),
                                                  u(Status::kExhausted)));
  EXPECT_EQ(r.outcome, Outcome::kExhausted);
  EXPECT_FALSE(r.w->usable());
}

// A nested DROP that answers INTERNAL: the worker is retired after the
// top-level request, which keeps its own outcome. `tainted` is read after
// the top-level RESULT only.
TEST_F(MleWorkerProtocol, RetirementAfterTheTopLevelRequest) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script()
                     .arm(Op::kDrop, fail_with(Status::kInternal))
                     .message(Op::kSql, execute())
                     .message(Op::kSql, execute())
                     .answer());
  prepare(*w, 2, Script().tainted().answer());
  int statements = 0;
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    std::string e;
    ResultMsg nested;
    if (statements++ == 0) {
      EXPECT_EQ(w->drop(9, &e), Outcome::kInternal);
    } else {
      // The worker is still in use, though it is to be retired and tainted.
      EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kOk) << e;
    }
    EXPECT_TRUE(w->usable());
    r->kind = SqlResultKind::kOk;
  };
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(statements, 2);
  EXPECT_FALSE(w->usable());
  EXPECT_NE(w->failure().find("INTERNAL to DROP"), std::string::npos)
      << w->failure();
}

// §5.1, §11.5: TAINTED is bit 1 of msg.flags of a RESULT, alike in all its
// chunks: the request keeps its outcome and the worker is retired when the
// top-level request is over. In any other message it is a violation.
TEST_F(MleWorkerProtocol, TaintedFlag) {
  const std::string good = bytes_of(good_result());
  Ran r = run(Where::kCall, raw_result(good, 0, wire::kTainted));
  ASSERT_NE(r.w, nullptr);
  EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
  EXPECT_EQ(*r.result.ret, Value::i64(42));
  EXPECT_FALSE(r.w->usable());
  EXPECT_NE(r.w->failure().find("retired"), std::string::npos);
  EXPECT_TRUE(r.w->process().wait_exit(2000));
  // A RESULT of several chunks, each with the flag.
  ResultMsg big = good_result();
  big.console_out.assign(3 * kArena, 'o');
  r = run(Where::kCall, Script().tainted().result(big));
  EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
  EXPECT_EQ(r.result.console_out.size(), 3u * kArena);
  EXPECT_FALSE(r.w->usable());
  // The RESULT of a request without guest code retires as well.
  r = run(Where::kDrop, raw_result(bytes_of(ResultMsg()), 0, wire::kTainted));
  EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
  EXPECT_FALSE(r.w->usable());

  // The chunks of a RESULT must agree on it.
  const std::string full(kArena, 'x');
  violation("TAINTED in the first chunk only", Where::kCall,
            raw_result(full, 0, kMore | wire::kTainted, kArena, kArena + 10)
                .wait()
                .post(u(Op::kResult), 0, 0, "0123456789", 10, kArena + 10),
            "chunk does not continue the message");
  violation("TAINTED in the last chunk only", Where::kCall,
            raw_result(full, 0, kMore, kArena, kArena + 10)
                .wait()
                .post(u(Op::kResult), 0, wire::kTainted, "0123456789", 10,
                      kArena + 10),
            "chunk does not continue the message");
  // Elsewhere the flag is a violation.
  const char *kElsewhere = "TAINTED in a message that is not a RESULT";
  violation("TAINTED in an SQL", Where::kCall,
            Script().post(u(Op::kSql), 0, wire::kTainted, bytes_of(execute())),
            kElsewhere);
  violation("TAINTED in a LIBRARY", Where::kCall,
            Script().post(u(Op::kLibrary), 0, wire::kTainted,
                          bytes_of(library())),
            kElsewhere);
  violation("TAINTED in a CONTINUE", Where::kContinue,
            Script().post(u(Op::kContinue), 0, wire::kTainted, "", 0, 0),
            kElsewhere);
}

// §10.2: KEEP (flags bit 1 of an SQL message) is not implemented by this
// host: the server's statement service does not let a statement outlive the
// stored program call that prepared it. The bit is an unknown flag.
TEST_F(MleWorkerProtocol, KeepIsNotAccepted) {
  SqlMsg m = action(SqlAction::kPrepare);
  m.statement = "SELECT 1";
  m.flags = 2;
  violation("KEEP", Where::kCall, sql(m).answer(), "bad SQL");
}

// §9.8: OPTIONS goes only to a worker in which no id is prepared; the
// options in force end with RESET.
TEST_F(MleWorkerProtocol, OptionsOnlyWithNothingPrepared) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  std::string err;
  // Nothing is prepared yet: the host may still replace the options.
  EXPECT_EQ(w->options(OptionsMsg(), nullptr, &err), Outcome::kOk) << err;
  prepare(*w, 1, Script().answer());
  OptionsMsg other;
  other.time_zone = "+05:00";
  EXPECT_EQ(w->options(other, nullptr, &err), Outcome::kRefused);
  EXPECT_NE(err.find("nothing is prepared"), std::string::npos) << err;
  EXPECT_TRUE(w->options_in_force());
  // DROP of the last routine: nothing is prepared again.
  EXPECT_EQ(w->drop(1, &err), Outcome::kOk) << err;
  EXPECT_EQ(w->options(other, nullptr, &err), Outcome::kOk) << err;
  prepare(*w, 2, Script().answer());
  EXPECT_EQ(w->reset(&err), Outcome::kOk) << err;
  EXPECT_FALSE(w->options_in_force());
  ResultMsg res;
  EXPECT_EQ(w->prepare(routine(3, ""), hooks, &res, &err), Outcome::kRefused);
  EXPECT_EQ(w->options(other, nullptr, &err), Outcome::kOk) << err;
  EXPECT_EQ(w->prepare(routine(3, ""), hooks, &res, &err), Outcome::kOk) << err;
}

// The hook interface: sql_into() is told the id its reply has if it is an
// ERROR, and sql_refused() whether the action had been given to sql_into().
// An ERROR's SQLSTATE, number and message are made what §10.4 says.
TEST_F(MleWorkerProtocol, HookErrorIdsAndRefusals) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script()
                     .message(Op::kSql, execute("a", 1))
                     .message(Op::kSql, execute("b", 2))
                     .message(Op::kSql, execute("c", 3))
                     .log()
                     .message(Op::kSql, execute("d", 4))
                     .answer());
  int n = 0;
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    r->kind = n++ == 1 || n == 4 ? SqlResultKind::kError : SqlResultKind::kOk;
    // What a hook must not send: the Worker repairs it.
    r->sql_errno = 0;
    r->sqlstate = "bad";
    r->message = "caf\xC3 broken";
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  // Each SQL message takes the next id of the top-level request, whether
  // its reply is an ERROR (the second and fourth) or not.
  EXPECT_EQ(hooks.error_ids, (std::vector<uint64_t>{1, 2, 3, 4}));
  const auto log = parse_log(res.console_out);
  ASSERT_EQ(log.size(), 2u);  // the CALL and the reply to the fourth
  SqlResultMsg sent;
  ASSERT_TRUE(decode(log[1].body, &sent, &err)) << err;
  EXPECT_EQ(sent.kind, SqlResultKind::kError);
  EXPECT_EQ(sent.error_id, 4u);
  EXPECT_EQ(sent.sqlstate, "HY000");
  EXPECT_EQ(sent.sql_errno, 1105u);
  EXPECT_EQ(sent.message, "caf\xEF\xBF\xBD broken");
  EXPECT_TRUE(hooks.refused.empty());

  // Refusals: before the hook ran (serving PREPARE), and after it (the
  // statement was interrupted while it ran).
  hooks.on_sql = nullptr;
  hooks.error_ids.clear();
  EXPECT_EQ(w->prepare(routine(2, "prepare:" + sql(execute()).answer().hex()),
                       hooks, &res, &err),
            Outcome::kOk)
      << err;
  ASSERT_EQ(hooks.refused.size(), 1u);
  EXPECT_FALSE(hooks.refused[0].ran);
  EXPECT_EQ(hooks.refused[0].error_id, 1u);
  hooks.refused.clear();
  prepare(*w, 3, Script().message(Op::kSql, execute()).answer());
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    w->interrupt();
    r->kind = SqlResultKind::kOk;
  };
  EXPECT_EQ(w->call(3, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_TRUE(w->interrupted());
  ASSERT_EQ(hooks.refused.size(), 1u);
  EXPECT_EQ(hooks.refused[0].why, SqlRefusal::kInterrupted);
  EXPECT_TRUE(hooks.refused[0].ran);
}

// The id an ERROR has is the one its hook was told, also when the hook ran
// nested requests whose statements failed meanwhile; each is held (§10.4,
// §10.8).
TEST_F(MleWorkerProtocol, ErrorIdsWithNestedRequests) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().message(Op::kSql, execute()).answer());
  prepare(*w, 2, Script().log().message(Op::kSql, execute()).answer());
  std::map<uint32_t, uint64_t> told;  // by depth
  std::string nested_log;
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    const uint64_t mine = hooks.error_ids.back();
    const uint32_t depth = w->depth();
    if (depth == 1) {
      ResultMsg nested;
      std::string e;
      EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kOk) << e;
      nested_log = nested.console_out;
    }
    told[depth] = mine;
    r->kind = SqlResultKind::kError;
    r->sql_errno = 1000 + depth;
    r->sqlstate = "HY000";
    r->message = "at depth " + std::to_string(depth);
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  ASSERT_EQ(told.size(), 2u);
  EXPECT_LT(told[1], told[2]);  // the outer statement's id came first
  // The nested statement's reply, as the worker received it.
  const auto log = parse_log(nested_log);
  ASSERT_EQ(log.size(), 2u);  // the nested CALL and the reply
  SqlResultMsg sent;
  ASSERT_TRUE(decode(log[1].body, &sent, &err)) << err;
  EXPECT_EQ(sent.error_id, told[2]);
  EXPECT_EQ(sent.sql_errno, 1002u);
  // Both are held under the ids their hooks were told (the reply carries
  // the id it is held under).
  for (const auto &[depth, id] : told) {
    const HeldError *held = w->held_error(id);
    ASSERT_NE(held, nullptr) << depth;
    EXPECT_EQ(held->message, "at depth " + std::to_string(depth));
  }
}

// §11.1 step 1 with the atomic state: a thread that asks for interrupts all
// the time while requests come and go. A record is made only between the
// post of a request's first chunk and the receipt of its RESULT, so an
// INTERRUPT names only an interruptible request that was sent, and no record
// appears while the worker is idle.
TEST_F(MleWorkerProtocol, InterruptStateUnderAKillerThread) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().answer());
  prepare(*w, 2, Script().drain_control().answer());
  // call_seq: OPTIONS 1, the PREPAREs 2 and 3; then the loop.
  uint64_t seq = 3;
  std::set<uint64_t> calls, others;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> asked{0};
  std::thread killer([&] {
    while (!stop.load()) {
      w->interrupt();
      asked++;
    }
  });
  int recorded = 0;
  const int kRounds = 20000;
  for (int i = 0; i < kRounds && w->usable(); i++) {
    if (i % 7 == 3) {
      // Not interruptible: no INTERRUPT may name it.
      ASSERT_EQ(w->drop(1000 + i, &err), Outcome::kOk) << err;
      others.insert(++seq);
      EXPECT_FALSE(w->interrupted());
      continue;
    }
    ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
    calls.insert(++seq);
    // Idle: the record of the request that is over stays as it is.
    const bool was = w->interrupted();
    recorded += was;
    for (int k = 0; k < 20; k++) w->interrupt();
    ASSERT_EQ(w->interrupted(), was);
  }
  stop = true;
  killer.join();
  ASSERT_TRUE(w->usable()) << w->failure();
  EXPECT_GT(asked.load(), 0u);
  // What the worker got on its control socket.
  ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  size_t datagrams = 0;
  for (const Logged &l : parse_log(res.console_out)) {
    ASSERT_EQ(l.h.op, 0u);
    ASSERT_EQ(l.body.size(), wire::kInterruptSize);
    uint64_t named;
    memcpy(&named, l.body.data() + 1, sizeof named);
    EXPECT_TRUE(calls.contains(named)) << "INTERRUPT(" << named << ")";
    EXPECT_FALSE(others.contains(named));
    datagrams++;
  }
  std::cout << "[ INFO     ] " << kRounds << " requests, " << asked.load()
            << " interrupt() calls, " << recorded << " records, " << datagrams
            << " INTERRUPTs still queued\n";
  // After the storm the worker serves on, and a request is interrupted as
  // ever.
  prepare(*w, 5, Script().await_interrupt().result(failed("none", "stopped"),
                                                   u(Status::kInterrupted)));
  std::thread late([&] {
    while (!w->interrupted()) w->interrupt();
  });
  EXPECT_EQ(w->call(5, 0, {}, hooks, &res, &err), Outcome::kInterrupted);
  late.join();
  EXPECT_TRUE(w->usable());
}

TEST_F(MleWorkerProtocol, StatsAreReplaced) {
  Ran r = legal("stats", Where::kCall, Script().bad_stats().answer(),
                Outcome::kOk);
  const WorkerStats s = r.w->stats();
  EXPECT_EQ(s.heap_max, 100u);
  EXPECT_EQ(s.heap_used, 100u);
  EXPECT_EQ(s.state, 0u);
  // Nothing is decided by the statistics (§3.1): the worker stays in use.
  EXPECT_TRUE(r.w->usable());
}

TEST_F(MleWorkerProtocol, MailboxCannotBeResized) {
  legal("resize", Where::kCall, Script().resize().answer(), Outcome::kOk);
}

// ---------------------------------------------------------------- dialogue

// §8.1, §10: SQL and LIBRARY while serving a CALL; the replies reach the
// worker as the hooks gave them, with error ids of the host.
TEST_F(MleWorkerProtocol, SqlAndLibrary) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  SqlMsg many = action(SqlAction::kExecuteMany, 4);
  many.params = {Value::i64(1), Value::i64(2)};
  many.row_width = 1;
  prepare(*w, 1,
          Script()
              .log()
              .message(Op::kSql, execute("SELECT 1", 3))
              .message(Op::kSql, action(SqlAction::kFetch, 3))
              .message(Op::kSql, many)
              .message(Op::kSql, action(SqlAction::kNextResult, 3))
              .message(Op::kSql, action(SqlAction::kClose, 3))
              .message(Op::kLibrary, library("db", "lib"))
              .message(Op::kLibrary, library("db", "other"))
              .answer());
  hooks.on_sql = [](const SqlMsg &q, SqlResultMsg *r) {
    switch (q.action) {
      case SqlAction::kExecute: {
        r->kind = SqlResultKind::kRows;
        ParamType t;
        t.tag = Tag::kString;
        t.type_name = "VARCHAR";
        r->columns = {Column("c", "t", t)};
        // A row value of the server may be ill-formed text (§7.1).
        r->rows = {Value::string("caf\xe9"), Value::null()};
        r->total_rows = 3;
        r->more = true;
        break;
      }
      case SqlAction::kFetch:
        r->kind = SqlResultKind::kError;
        r->sql_errno = 1317;
        r->sqlstate = "70100";
        r->message = "first";
        r->error_id = 999;  // the Worker assigns the id
        break;
      case SqlAction::kExecuteMany:
        r->kind = SqlResultKind::kError;
        r->sql_errno = 1062;
        r->sqlstate = "23000";
        r->message = "second";
        r->failed_row = 1;
        break;
      case SqlAction::kNextResult:
        r->kind = SqlResultKind::kEnd;
        break;
      default:
        // A CLOSE is answered OK whatever the hook says.
        r->kind = SqlResultKind::kError;
        r->sql_errno = 1;
        r->sqlstate = "HY000";
    }
  };
  hooks.on_library = [](const LibraryMsg &q, LibraryResultMsg *r) {
    if (q.name != "lib") return;  // REFUSED
    r->status = wire::LibraryStatus::kBody;
    r->language = "none";
    r->body = "the body";
    r->version = 5;
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(hooks.ran.size(), 5u);
  EXPECT_TRUE(hooks.refused.empty());
  ASSERT_EQ(hooks.libraries.size(), 2u);
  EXPECT_EQ(hooks.libraries[0].name, "lib");
  EXPECT_EQ(hooks.bytes_held, 0u);
  EXPECT_GT(hooks.bytes_peak, 0u);
  const auto log = parse_log(res.console_out);
  ASSERT_EQ(log.size(), 8u);  // the CALL and seven replies
  for (const Logged &l : log) {
    EXPECT_EQ(l.h.depth, 0u);
    EXPECT_EQ(l.h.call_seq, log[0].h.call_seq);
    EXPECT_EQ(l.h.status, 0u);
    EXPECT_EQ(l.h.reserved, 0u);
  }
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 5u);
  EXPECT_EQ(r[0].kind, SqlResultKind::kRows);
  EXPECT_EQ(r[0].rows[0].as_text(), "caf\xe9");
  EXPECT_EQ(r[1].kind, SqlResultKind::kError);
  // Each SQL message takes the next id; an ERROR carries its message's.
  EXPECT_EQ(r[1].error_id, 2u);
  EXPECT_EQ(r[1].failed_row, wire::kNoRow);
  EXPECT_EQ(r[2].error_id, 3u);
  EXPECT_EQ(r[2].failed_row, 1u);
  EXPECT_EQ(r[3].kind, SqlResultKind::kEnd);
  EXPECT_EQ(r[4].kind, SqlResultKind::kOk);
  // The ERRORs are held for the routine's final error (§10.8).
  const HeldError *held = w->held_error(3);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->sql_errno, 1062u);
  EXPECT_EQ(held->sqlstate, "23000");
  EXPECT_EQ(held->message, "second");
  ASSERT_NE(w->held_error(2), nullptr);
  EXPECT_EQ(w->held_error(2)->message, "first");
  EXPECT_EQ(w->held_error(0), nullptr);
  EXPECT_EQ(w->held_error(1), nullptr);  // the id of a reply that was no ERROR
  EXPECT_EQ(w->held_error(4), nullptr);
  EXPECT_EQ(w->held_error(999), nullptr);
  LibraryResultMsg lib;
  ASSERT_EQ(log[6].h.op, u(Op::kLibraryResult));
  ASSERT_TRUE(decode(log[6].body, &lib, &err)) << err;
  EXPECT_EQ(lib.status, wire::LibraryStatus::kBody);
  EXPECT_EQ(lib.body, "the body");
  EXPECT_EQ(lib.version, 5u);
  ASSERT_TRUE(decode(log[7].body, &lib, &err)) << err;
  EXPECT_EQ(lib.status, wire::LibraryStatus::kRefused);
  EXPECT_EQ(lib.version, 0u);
  // The next request starts with new ids and holds nothing.
  ASSERT_EQ(w->drop(5, &err), Outcome::kOk);
  EXPECT_EQ(w->held_error(2), nullptr);
}

// §10.8: the host holds at least the 16 most recent ERRORs of a top-level
// request.
TEST_F(MleWorkerProtocol, HeldErrors) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().loop(40, sql(execute())).answer());
  int n = 0;
  hooks.on_sql = [&n](const SqlMsg &, SqlResultMsg *r) {
    r->kind = SqlResultKind::kError;
    r->sql_errno = 1000 + ++n;
    r->sqlstate = "HY000";
    r->message = "error " + std::to_string(n);
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  for (uint64_t id = 1; id <= 40; id++) {
    const HeldError *e = w->held_error(id);
    if (id <= 24) {
      EXPECT_EQ(e, nullptr) << id;
    } else {
      ASSERT_NE(e, nullptr) << id;
      EXPECT_EQ(e->sql_errno, 1000 + id);
      EXPECT_EQ(e->message, "error " + std::to_string(id));
    }
  }
}

// §8.1, §10.1: SQL while serving PREPARE is answered ERROR (CLOSE: OK)
// without being run, and is no violation; LIBRARY is served.
TEST_F(MleWorkerProtocol, SqlWhileServingPrepare) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  SqlMsg many = action(SqlAction::kExecuteMany, 2);
  many.params = {Value::i64(1)};
  many.row_width = 1;
  const Script script = Script()
                            .log()
                            .message(Op::kSql, execute())
                            .message(Op::kSql, many)
                            .message(Op::kSql, action(SqlAction::kNextResult))
                            .message(Op::kSql, action(SqlAction::kClose, 6))
                            .message(Op::kLibrary, library())
                            .answer();
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->prepare(routine(1, "prepare:" + script.hex()), hooks, &res,
                       &err),
            Outcome::kOk)
      << err;
  EXPECT_TRUE(hooks.ran.empty());
  EXPECT_EQ(hooks.libraries.size(), 1u);
  ASSERT_EQ(hooks.refused.size(), 4u);
  for (const auto &x : hooks.refused)
    EXPECT_EQ(x.why, SqlRefusal::kPreparing);
  EXPECT_EQ(hooks.refused[3].action, SqlAction::kClose);
  EXPECT_EQ(hooks.refused[3].stmt_id, 6u);
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 4u);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(r[i].kind, SqlResultKind::kError) << i;
    EXPECT_EQ(r[i].error_id, uint64_t(i + 1));
    EXPECT_NE(r[i].sql_errno, 0u);
    EXPECT_EQ(r[i].sqlstate.size(), 5u);
  }
  EXPECT_EQ(r[0].failed_row, wire::kNoRow);
  EXPECT_EQ(r[1].failed_row, 0u);  // EXECUTE_MANY: no row was executed
  EXPECT_EQ(r[3].kind, SqlResultKind::kOk);
  EXPECT_TRUE(w->usable());
  EXPECT_TRUE(w->is_prepared(1));
}

// §7.2: an SQL parameter that is not finite gives SQL_RESULT ERROR.
TEST_F(MleWorkerProtocol, NonFiniteParametersAreErrors) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  SqlMsg q = execute("SELECT ?");
  q.params = {Value::i64(1), Value::f64(std::nan(""))};
  SqlMsg many = action(SqlAction::kExecuteMany, 2);
  many.params = {Value::vector({1.0f}),
                 Value::vector({std::numeric_limits<float>::infinity()})};
  many.row_width = 1;
  SqlMsg fine = execute("SELECT ?");
  fine.params = {Value::f64(-0.0)};
  prepare(*w, 1, Script().log().loop(1, sql(q)).loop(1, sql(many)).loop(
                     1, sql(fine)).answer());
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(hooks.ran.size(), 1u);
  ASSERT_EQ(hooks.refused.size(), 2u);
  EXPECT_EQ(hooks.refused[0].why, SqlRefusal::kNonFiniteParameter);
  EXPECT_EQ(hooks.refused[1].action, SqlAction::kExecuteMany);
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0].kind, SqlResultKind::kError);
  EXPECT_EQ(r[0].failed_row, wire::kNoRow);
  EXPECT_EQ(r[1].kind, SqlResultKind::kError);
  EXPECT_EQ(r[1].failed_row, 0u);
  EXPECT_EQ(r[2].kind, SqlResultKind::kOk);
  EXPECT_TRUE(w->usable());
}

// §5.3, §10.7: an SQL message over the bound on request bytes is taken in
// and discarded; only its action and statement id are kept, the rest is not
// checked; the reply is ERROR. CLOSE is never refused this way.
TEST_F(MleWorkerProtocol, RequestBytesBound) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  hooks.bytes_bound = 1000;
  std::string garbage(3 * kArena + 17, '\xff');  // not a well-formed SQL body
  garbage[0] = static_cast<char>(SqlAction::kExecutePrepared);
  const uint64_t id = 0x0102030405060708;
  memcpy(garbage.data() + 1, &id, sizeof id);
  SqlMsg large = execute(std::string(1001, ' '), 2);
  prepare(*w, 1, Script()
                     .log()
                     .message(Op::kSql, 0, garbage)
                     .message(Op::kSql, 0, garbage.substr(0, 1001))
                     .message(Op::kSql, large)
                     .message(Op::kSql, action(SqlAction::kClose, id))
                     .message(Op::kSql, execute())
                     .answer());
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  ASSERT_EQ(hooks.refused.size(), 3u);
  EXPECT_EQ(hooks.refused[0].why, SqlRefusal::kRequestTooLarge);
  EXPECT_EQ(hooks.refused[0].action, SqlAction::kExecutePrepared);
  EXPECT_EQ(hooks.refused[0].stmt_id, id);
  EXPECT_EQ(hooks.refused[2].action, SqlAction::kExecute);
  EXPECT_EQ(hooks.refused[2].stmt_id, 2u);
  ASSERT_EQ(hooks.ran.size(), 2u);
  EXPECT_EQ(hooks.ran[0], SqlAction::kClose);
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 5u);
  for (int i = 0; i < 3; i++) EXPECT_EQ(r[i].kind, SqlResultKind::kError) << i;
  EXPECT_EQ(r[3].kind, SqlResultKind::kOk);
  EXPECT_EQ(r[4].kind, SqlResultKind::kOk);
  EXPECT_EQ(hooks.bytes_held, 0u);
  EXPECT_LE(hooks.bytes_peak, 1000u);
  EXPECT_TRUE(w->usable());
  // The bytes of the statements in progress are counted over all depths.
  hooks.refused.clear();
  hooks.bytes_bound = 150;
  prepare(*w, 2, Script().loop(1, sql(execute(std::string(60, ' ')))).answer());
  int depth = 0;
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r2) {
    ResultMsg nested;
    std::string e;
    if (++depth < 3) {
      EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kOk) << e;
    }
    r2->kind = SqlResultKind::kOk;
  };
  ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(depth, 1);  // the second level's message was over the bound
  EXPECT_EQ(hooks.refused.size(), 1u);
  EXPECT_EQ(hooks.bytes_held, 0u);
}

// §8.1, §8.2, §10.6: nested PREPARE, CALL and DROP while the worker waits
// for an SQL_RESULT, to depth 31; no request at depth 32.
TEST_F(MleWorkerProtocol, NestedRequests) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().log().message(Op::kSql, execute()).answer());
  uint32_t deepest = 0, refused = 0;
  std::vector<uint32_t> depths;
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    deepest = std::max(deepest, w->depth());
    ResultMsg nested;
    std::string e;
    const Outcome o = w->call(1, 0, {}, hooks, &nested, &e);
    if (o == Outcome::kRefused) {
      refused++;
      EXPECT_NE(e.find("too many nested"), std::string::npos) << e;
      EXPECT_EQ(w->drop(1, &e), Outcome::kRefused);
      // What is top-level only is not sent either.
    } else {
      EXPECT_EQ(o, Outcome::kOk) << e;
      // The worker has one log: the innermost request returns all of it.
      for (const Logged &l : parse_log(nested.console_out))
        if (l.h.op == u(Op::kCall)) depths.push_back(l.h.depth);
    }
    EXPECT_EQ(w->reset(&e), Outcome::kRefused);
    EXPECT_EQ(w->parse_library({"none", "l", "x"}, hooks, &nested, &e),
              Outcome::kRefused);
    EXPECT_EQ(w->options(OptionsMsg(), nullptr, &e), Outcome::kRefused);
    r->kind = SqlResultKind::kOk;
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(deepest, 32u);  // requests in progress: depths 0 to 31
  EXPECT_EQ(refused, 1u);
  // Every CALL carried its depth: 0 for the top-level one, up to 31.
  ASSERT_EQ(depths.size(), 32u);
  for (uint32_t d = 0; d < 32; d++) EXPECT_EQ(depths[d], d);
  EXPECT_EQ(w->depth(), 0u);
  EXPECT_TRUE(w->usable());

  // Nested PREPARE and DROP; a nested request is possible only while the
  // worker waits for an SQL_RESULT: not from the library hook.
  prepare(*w, 2, Script()
                     .message(Op::kSql, execute())
                     .message(Op::kLibrary, library())
                     .answer());
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg nested;
    std::string e;
    EXPECT_EQ(w->prepare(routine(3, ""), hooks, &nested, &e), Outcome::kOk);
    EXPECT_EQ(w->call(3, 0, {}, hooks, &nested, &e), Outcome::kOk) << e;
    EXPECT_EQ(w->drop(3, &e), Outcome::kOk) << e;
    // A nested PREPARE may replace the routine that is running: its RESULT
    // is checked against the parameters it was called with.
    EXPECT_EQ(w->prepare(routine(2, "", false), hooks, &nested, &e),
              Outcome::kOk);
    r->kind = SqlResultKind::kOk;
  };
  hooks.on_library = [&](const LibraryMsg &, LibraryResultMsg *) {
    std::string e;
    ResultMsg nested;
    EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kRefused);
  };
  ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_EQ(*res.ret, Value::i64(42));
  EXPECT_FALSE(w->is_prepared(3));
}

// A DROP asked for where no request can be sent (the worker is busy and the
// caller is not in the SQL hook of its innermost request): the id is not
// prepared at once, and the DROP goes out before the next top-level request.
TEST_F(MleWorkerProtocol, DropWhileBusy) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().message(Op::kLibrary, library()).answer());
  prepare(*w, 2, Script().answer());
  prepare(*w, 3, Script().answer());
  hooks.on_library = [&](const LibraryMsg &, LibraryResultMsg *) {
    std::string e;
    EXPECT_EQ(w->drop(2, &e), Outcome::kOk) << e;
    EXPECT_FALSE(w->is_prepared(2));
    EXPECT_EQ(w->drop(3, &e), Outcome::kOk) << e;
  };
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_FALSE(w->is_prepared(2));
  EXPECT_FALSE(w->is_prepared(3));
  // The host does not call what is not prepared.
  EXPECT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kRefused);
  // The DROPs that waited go out before the next top-level request; the id
  // can be prepared and called again after them.
  prepare(*w, 3, Script().answer());
  EXPECT_TRUE(w->is_prepared(3));
  EXPECT_EQ(w->call(3, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  EXPECT_TRUE(w->usable());
}

// §8.1: a RESULT at a nested depth with the outer request's shape, and an
// SQL of the outer request while a nested one is served.
TEST_F(MleWorkerProtocol, NestedViolations) {
  auto w = start();
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().message(Op::kSql, execute()).answer());
  // The nested routine answers at the outer depth.
  prepare(*w, 2, raw_result(bytes_of(good_result()), 0, 0, mle_test::kAuto,
                            mle_test::kAuto, -1));
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg nested;
    std::string e;
    EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kViolation);
    EXPECT_NE(e.find("wrong request"), std::string::npos) << e;
    r->kind = SqlResultKind::kOk;
  };
  // The outer request reports the nested one's outcome, not kDied; the loss is reported once
  // (MVP review HOST-2).
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kViolation);
  EXPECT_NE(err.find("wrong request"), std::string::npos) << err;
  EXPECT_FALSE(w->usable());
  EXPECT_EQ(w->depth(), 0u);
  EXPECT_TRUE(w->first_report_of_failure());
  EXPECT_FALSE(w->first_report_of_failure());
}

// ---------------------------------------------------------------- death

// §11.4: the worker's death is noticed; a RESULT it posted just before is
// not lost.
TEST_F(MleWorkerProtocol, Death) {
  Ran r = run(Where::kCall, Script().exit(7));
  ASSERT_NE(r.w, nullptr);
  EXPECT_EQ(r.outcome, Outcome::kDied);
  EXPECT_EQ(r.error, "the MLE worker died: exited with status 7");
  std::string err;
  ResultMsg res;
  EXPECT_EQ(r.w->call(1, 0, {}, hooks, &res, &err), Outcome::kDied);
  EXPECT_EQ(err, "the MLE worker died: exited with status 7");

  // A last RESULT, complete in one chunk, is processed normally.
  ResultMsg last = good_result();
  last.console_out = "last words";
  for (int i = 0; i < 20; i++) {
    r = run(Where::kCall, Script().result(last).exit(0));
    ASSERT_NE(r.w, nullptr);
    EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
    EXPECT_EQ(r.result, last);
    EXPECT_TRUE(r.w->process().wait_exit(2000));
    EXPECT_EQ(r.w->call(1, 0, {}, hooks, &res, &err), Outcome::kDied);
  }
  // Any other message of a dead worker is dropped.
  r = run(Where::kCall, Script()
                            .post(u(Op::kSql), 0, 0, bytes_of(execute()))
                            .exit(4));
  EXPECT_EQ(r.outcome, Outcome::kDied);
  EXPECT_EQ(r.error, "the MLE worker died: exited with status 4");
  // After an interrupt was requested, a worker that ends was ended by it.
  hooks.kill = false;
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  prepare(*w, 1, Script().await_interrupt().exit(5));
  std::thread killer = kill_after(50);
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kKilled);
  killer.join();
  EXPECT_NE(err.find("ended after the interrupt (exited with status 5)"),
            std::string::npos)
      << err;
  EXPECT_TRUE(w->interrupted());
}

// ---------------------------------------------------------------- deadlines

// §11.2: DROP, RESET and OPTIONS have one deadline for the whole exchange.
TEST_F(MleWorkerProtocol, Deadlines) {
  WorkerOptions o;
  o.control_timeout_ms = 300;
  for (const Where where : {Where::kDrop, Where::kReset, Where::kOptions}) {
    const auto t0 = Clock::now();
    Ran r = run(where, Script().sleep(60000), o);
    ASSERT_NE(r.w, nullptr);
    const int64_t ms = ms_since(t0);
    EXPECT_EQ(r.outcome, Outcome::kTimedOut) << r.error;
    EXPECT_NE(r.error.find("within 300 ms"), std::string::npos) << r.error;
    EXPECT_GE(ms, 300);
    EXPECT_LT(ms, 1500);
    EXPECT_FALSE(r.w->usable());
    EXPECT_EQ(r.w->process().exit_info(2000).describe(),
              "killed by signal 9 (Killed)");
  }
  // An answer within the deadline is in time, however late.
  Ran r = run(Where::kReset, Script().sleep(200).answer(), o);
  EXPECT_EQ(r.outcome, Outcome::kOk) << r.error;
  EXPECT_TRUE(r.w->usable());
  // A kill of the session does not end these requests: they have the
  // deadline (§11.1).
  auto w = start(o);
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  const Script arm =
      Script().arm(Op::kDrop, Script().sleep(100).answer()).answer();
  ASSERT_EQ(w->prepare(routine(1, "prepare:" + arm.hex()), hooks, &res,
                       &err),
            Outcome::kOk)
      << err;
  hooks.kill = true;
  EXPECT_EQ(w->drop(1, &err), Outcome::kOk) << err;
  EXPECT_FALSE(w->interrupted());
  EXPECT_EQ(w->reset(&err), Outcome::kOk) << err;
  hooks.kill = false;
}

// A worker cannot stretch the exchange over chunks: every post it makes in
// answer to a DROP is the whole RESULT or a violation, at once.
TEST_F(MleWorkerProtocol, DeadlineCannotBeStretched) {
  WorkerOptions o;
  o.control_timeout_ms = 400;
  const std::string full(kArena, 'f');
  // Each step stays under the deadline; together they would pass it.
  const Script stalling = Script()
                              .sleep(250)
                              .post(u(Op::kResult), 0, kMore, full, kArena,
                                    4 * kArena)
                              .wait()
                              .sleep(250)
                              .post(u(Op::kResult), 0, kMore, full, kArena,
                                    4 * kArena);
  const auto t0 = Clock::now();
  Ran r = run(Where::kDrop, stalling, o);
  ASSERT_NE(r.w, nullptr);
  EXPECT_EQ(r.outcome, Outcome::kViolation) << r.error;
  EXPECT_NE(r.error.find("RESULT larger than its limit"), std::string::npos);
  EXPECT_LT(ms_since(t0), 400 + 250);
  // A nested DROP has its own deadline; missing it ends the worker, and the
  // top-level request with it.
  auto w = start(o);
  ASSERT_NE(w, nullptr);
  prepare(*w, 1, Script()
                     .arm(Op::kDrop, Script().sleep(60000))
                     .message(Op::kSql, execute())
                     .answer());
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *res) {
    std::string e;
    const auto t1 = Clock::now();
    EXPECT_EQ(w->drop(5, &e), Outcome::kTimedOut) << e;
    EXPECT_GE(ms_since(t1), 400);
    EXPECT_LT(ms_since(t1), 1500);
    res->kind = SqlResultKind::kOk;
  };
  ResultMsg res;
  std::string err;
  // The top-level request reports the nested one's outcome (MVP review HOST-2).
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kTimedOut);
  EXPECT_NE(err.find("did not answer DROP"), std::string::npos) << err;
}

// ---------------------------------------------------------------- interrupts

// §11.1: the record is made between the first chunk's post and the
// RESULT's receipt, by interrupt() from any thread or by the session
// thread that finds itself killed; INTERRUPTED needs it.
TEST_F(MleWorkerProtocol, InterruptAnswered) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1,
          Script().await_interrupt().result(failed("none", "stopped"),
                                            u(Status::kInterrupted)));
  prepare(*w, 2, Script().answer());
  // Without a request in progress there is nothing to interrupt.
  w->interrupt();
  EXPECT_FALSE(w->interrupted());
  ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  for (int i = 0; i < 3; i++) {
    // From another thread, as the server's KILL does.
    std::thread killer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      w->interrupt();
      w->interrupt();  // several INTERRUPTs for one request are legal
    });
    const auto t0 = Clock::now();
    EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted);
    killer.join();
    EXPECT_LT(ms_since(t0), kGrace);
    EXPECT_EQ(err, "stopped");
    EXPECT_TRUE(w->interrupted());
    EXPECT_TRUE(w->usable());
    // The worker is in use again; the next request is not interrupted.
    ASSERT_EQ(w->call(2, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
    EXPECT_FALSE(w->interrupted());
  }
  // By the session's own kill flag.
  std::thread killer = kill_after(30);
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted);
  killer.join();
  EXPECT_TRUE(w->interrupted());
  hooks.kill = false;
  // PARSE_LIBRARY is interruptible too.
  const Script stop = Script().await_interrupt().result(
      failed("none"), u(Status::kInterrupted));
  killer = kill_after(30);
  EXPECT_EQ(w->parse_library({"none", "l", "script:" + stop.hex()}, hooks, &res,
                             &err),
            Outcome::kInterrupted);
  killer.join();
  EXPECT_TRUE(w->usable());
}

// §11.1 step 2: from the moment of notice every SQL_RESULT is ERROR (to
// CLOSE: OK), LIBRARY is answered REFUSED, and the hooks run nothing.
TEST_F(MleWorkerProtocol, NoNewWorkAfterAnInterrupt) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  SqlMsg many = action(SqlAction::kExecuteMany, 2);
  many.params = {Value::i64(1), Value::i64(2), Value::i64(3)};
  many.row_width = 1;
  prepare(*w, 1,
          Script()
              .log()
              .message(Op::kSql, execute())  // runs
              .await_interrupt()
              .message(Op::kSql, execute())
              .message(Op::kSql, many)
              .message(Op::kSql, action(SqlAction::kNextResult))
              .message(Op::kSql, action(SqlAction::kClose, 8))
              .message(Op::kLibrary, library())
              .answer());
  std::thread killer = kill_after(50);
  // The request completed otherwise: its RESULT is what it would have been.
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  killer.join();
  hooks.kill = false;
  EXPECT_TRUE(w->interrupted());
  EXPECT_EQ(hooks.ran.size(), 1u);
  EXPECT_TRUE(hooks.libraries.empty());
  ASSERT_EQ(hooks.refused.size(), 4u);
  for (const auto &x : hooks.refused)
    EXPECT_EQ(x.why, SqlRefusal::kInterrupted);
  EXPECT_EQ(hooks.refused[3].action, SqlAction::kClose);
  const auto log = parse_log(res.console_out);
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 5u);
  EXPECT_EQ(r[0].kind, SqlResultKind::kOk);
  for (int i = 1; i <= 3; i++) {
    EXPECT_EQ(r[i].kind, SqlResultKind::kError) << i;
    EXPECT_EQ(r[i].sql_errno, 1317u);
    // The i-th SQL message after the first: its id is i + 1.
    EXPECT_EQ(r[i].error_id, uint64_t(i + 1));
  }
  EXPECT_EQ(r[2].failed_row, 0u);
  EXPECT_EQ(r[4].kind, SqlResultKind::kOk);
  LibraryResultMsg lib;
  ASSERT_TRUE(decode(log.back().body, &lib, &err)) << err;
  EXPECT_EQ(lib.status, wire::LibraryStatus::kRefused);
  EXPECT_TRUE(w->usable());
}

// §11.1 step 2: a statement that was running and succeeded meanwhile is
// answered ERROR; no nested request is sent; a nested DROP or PREPARE that
// became necessary is not sent, the id is not prepared, and a top-level DROP
// goes out before the worker is used again.
TEST_F(MleWorkerProtocol, InterruptDuringAStatement) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  SqlMsg many = action(SqlAction::kExecuteMany, 2);
  many.params = {Value::i64(1), Value::i64(2), Value::i64(3)};
  many.row_width = 1;
  prepare(*w, 1, Script()
                     .log()
                     .message(Op::kSql, many)
                     .await_interrupt()
                     .result(failed("none"), u(Status::kInterrupted)));
  prepare(*w, 2, Script().log().answer());
  prepare(*w, 3, Script().answer());
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    hooks.kill = true;  // the session is killed while the statement runs
    ResultMsg nested;
    std::string e;
    EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kInterrupted);
    EXPECT_EQ(w->drop(2, &e), Outcome::kInterrupted);
    EXPECT_EQ(w->prepare(routine(3, ""), hooks, &nested, &e),
              Outcome::kInterrupted);
    r->kind = SqlResultKind::kOk;  // the statement itself succeeded
    r->affected_rows = 3;
  };
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted);
  hooks.kill = false;
  hooks.on_sql = nullptr;
  ASSERT_EQ(hooks.refused.size(), 1u);
  EXPECT_EQ(hooks.refused[0].why, SqlRefusal::kInterrupted);
  EXPECT_TRUE(w->usable());
  EXPECT_TRUE(w->is_prepared(1));
  EXPECT_FALSE(w->is_prepared(2));
  EXPECT_FALSE(w->is_prepared(3));
  // The two ids are dropped at top level before the next request: the
  // PREPARE that follows has a call_seq two further on.
  prepare(*w, 4, Script().log().answer());
  ASSERT_EQ(w->call(4, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  const auto log = parse_log(res.console_out);
  ASSERT_EQ(log.size(), 1u);
  // OPTIONS 1, PREPAREs 2 to 4, CALL 5, DROPs 6 and 7, PREPARE 8, CALL 9.
  EXPECT_EQ(log[0].h.call_seq, 9u);
}

// The reply of a statement that was running: ERROR with the rows executed.
TEST_F(MleWorkerProtocol, InterruptedStatementIsAnError) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  SqlMsg many = action(SqlAction::kExecuteMany, 2);
  many.params = {Value::i64(1), Value::i64(2), Value::i64(3)};
  many.row_width = 1;
  prepare(*w, 1, Script().log().message(Op::kSql, many).answer());
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    hooks.kill = true;
    r->kind = SqlResultKind::kOk;
    r->affected_rows = 3;
  };
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kOk) << err;
  hooks.kill = false;
  const auto r = replies(res.console_out);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].kind, SqlResultKind::kError);
  EXPECT_EQ(r[0].failed_row, 3u);  // all three rows were executed
  EXPECT_EQ(r[0].affected_rows, 0u);
  EXPECT_TRUE(w->interrupted());
}

// ---------------------------------------------------------------- the clock

// §11.1 step 3. G is kGrace; the session is killed 50 ms into the request.

// A worker that does not answer is ended when the clock reaches G.
TEST_F(MleWorkerProtocol, GraceSilentWorker) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().sleep(60000));
  std::thread killer = kill_after(50);
  const auto t0 = Clock::now();
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kKilled);
  const int64_t ms = ms_since(t0);
  killer.join();
  EXPECT_GE(ms, 50 + kGrace);
  EXPECT_LT(ms, 50 + kGrace + 500);
  EXPECT_NE(err.find("did not stop within 300 ms after the interrupt"),
            std::string::npos)
      << err;
  EXPECT_FALSE(w->usable());
  EXPECT_TRUE(w->interrupted());
  EXPECT_EQ(w->process().exit_info(2000).describe(),
            "killed by signal 9 (Killed)");
}

// A worker that keeps the host busy with small SQL after the interrupt buys
// no time: every reply is an ERROR the hooks do not run, and the clock ends
// it at G.
TEST_F(MleWorkerProtocol, GraceSqlStream) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  prepare(*w, 1, Script().await_interrupt().loop(100000000, sql(execute())));
  std::thread killer = kill_after(50);
  const auto t0 = Clock::now();
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kKilled);
  const int64_t ms = ms_since(t0);
  killer.join();
  EXPECT_GE(ms, 50 + kGrace);
  EXPECT_LT(ms, 50 + kGrace + 500);
  EXPECT_NE(err.find("did not stop within 300 ms"), std::string::npos) << err;
  EXPECT_TRUE(hooks.ran.empty());
  EXPECT_GT(hooks.refused.size(), 100u);
  EXPECT_EQ(hooks.bytes_held, 0u);
}

// The extension: at the moment of notice the worker has the turn and may
// still be taking in the host's last message; the limit grows by
// G x total_len / 16 MiB.
TEST_F(MleWorkerProtocol, GraceExtension) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  PrepareMsg p = routine(
      1, "call:" + Script()
                       .await_interrupt()
                       .sleep(2 * kGrace)
                       .result(failed("none"), u(Status::kInterrupted))
                       .hex());
  ParamType blob;
  blob.tag = Tag::kBytes;
  blob.type_name = "LONGBLOB";
  p.params = {{"a", ParamMode::kIn, blob}};
  ASSERT_EQ(w->prepare(p, hooks, &res, &err), Outcome::kOk) << err;
  // 32 MiB: the limit is 3 G. The kill comes when the transfer is over.
  const std::vector<Value> large = {Value::bytes(std::string(32u << 20, 'x'))};
  std::thread killer = kill_after(1000);
  EXPECT_EQ(w->call(1, 0, large, hooks, &res, &err), Outcome::kInterrupted)
      << err;
  killer.join();
  hooks.kill = false;
  EXPECT_TRUE(w->usable());
  // The same wait after a small message is beyond G.
  killer = kill_after(50);
  EXPECT_EQ(w->call(1, 0, {Value::bytes("x")}, hooks, &res, &err),
            Outcome::kKilled);
  killer.join();
  EXPECT_NE(err.find("did not stop within 300 ms"), std::string::npos) << err;
}

// Exception (b): the transfer of a RESULT is not on the clock; its turns
// are bounded one by one and together.
TEST_F(MleWorkerProtocol, GraceResultTransfer) {
  ResultMsg big = failed("none", "stopped");
  big.console_out.assign(4 * kArena + 100, 'c');  // five chunks
  const uint32_t interrupted = u(Status::kInterrupted);
  auto call = [&](const Script &script, std::string *err, int64_t *ms) {
    auto w = start(grace());
    if (!w) return Outcome::kRefused;
    ResultMsg res;
    prepare(*w, 1, script);
    hooks.kill = false;
    std::thread killer = kill_after(50);
    const auto t0 = Clock::now();
    const Outcome o = w->call(1, 0, {}, hooks, &res, err);
    *ms = ms_since(t0) - 50;
    killer.join();
    hooks.kill = false;
    EXPECT_EQ(w->usable(), o == Outcome::kInterrupted);
    if (o == Outcome::kInterrupted) {
      EXPECT_EQ(res.console_out.size(), big.console_out.size());
    }
    return o;
  };
  std::string err;
  int64_t ms;
  // 0.6 G on the clock, then four turns of 0.2 G inside the transfer: 1.4 G
  // after the interrupt, and within every bound.
  EXPECT_EQ(call(Script()
                     .await_interrupt()
                     .sleep(kGrace * 6 / 10)
                     .result(big, interrupted, kGrace * 2 / 10),
                 &err, &ms),
            Outcome::kInterrupted)
      << err;
  EXPECT_GE(ms, kGrace * 14 / 10);
  // One turn of 1.5 G: over the bound of a turn, G x (1 + 64 KiB / 16 MiB).
  EXPECT_EQ(call(Script().await_interrupt().result(big, interrupted,
                                                   kGrace * 15 / 10),
                 &err, &ms),
            Outcome::kKilled);
  EXPECT_NE(err.find("for one chunk of a message"), std::string::npos) << err;
  EXPECT_GE(ms, kGrace);
  EXPECT_LT(ms, kGrace + 400);
  // Turns of 0.6 G: each within its bound, the second over the transfer's,
  // G x (1 + total_len / 16 MiB).
  EXPECT_EQ(call(Script().await_interrupt().result(big, interrupted,
                                                   kGrace * 6 / 10),
                 &err, &ms),
            Outcome::kKilled);
  EXPECT_NE(err.find("for the chunks of a message"), std::string::npos) << err;
  EXPECT_GE(ms, kGrace);
  EXPECT_LT(ms, kGrace * 12 / 10 + 400);
  // The turn that produces the first chunk is not inside the transfer: it
  // counts on the clock.
  EXPECT_EQ(call(Script()
                     .await_interrupt()
                     .sleep(kGrace * 15 / 10)
                     .result(big, interrupted),
                 &err, &ms),
            Outcome::kKilled);
  EXPECT_NE(err.find("did not stop within"), std::string::npos) << err;
}

// Exception (b): a transfer of the host in progress at the moment of notice
// is excepted, and its turns are bounded.
TEST_F(MleWorkerProtocol, GraceHostTransfer) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  // The worker takes 1.5 G to answer the first chunk of the next message.
  const Script slow = Script().sleep(kGrace * 15 / 10).post(u(Op::kContinue), 0,
                                                             0, {}, 0, 0);
  ASSERT_EQ(w->prepare(routine(1, "prepare:" +
                                      Script()
                                          .arm(Op::kContinue, slow)
                                          .answer()
                                          .hex()),
                       hooks, &res, &err),
            Outcome::kOk)
      << err;
  std::thread killer = kill_after(50);
  const auto t0 = Clock::now();
  EXPECT_EQ(w->prepare(routine(2, std::string(3 * kArena, 'x')), hooks, &res,
                       &err),
            Outcome::kKilled);
  const int64_t ms = ms_since(t0) - 50;
  killer.join();
  EXPECT_NE(err.find("for one chunk of a message"), std::string::npos) << err;
  EXPECT_GE(ms, kGrace);
  EXPECT_LT(ms, kGrace + 400);
  // Without a kill the same pause is nothing.
  hooks.kill = false;
  w = start(grace());
  ASSERT_NE(w, nullptr);
  ASSERT_EQ(w->prepare(routine(1, "prepare:" +
                                      Script()
                                          .arm(Op::kContinue, slow)
                                          .answer()
                                          .hex()),
                       hooks, &res, &err),
            Outcome::kOk);
  EXPECT_EQ(w->prepare(routine(2, std::string(3 * kArena, 'x')), hooks, &res,
                       &err),
            Outcome::kOk)
      << err;
}

// Exception (a): the time the server takes to end a statement that was
// begun before the moment of notice is not the worker's.
TEST_F(MleWorkerProtocol, GraceLongStatement) {
  auto w = start(grace());
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  const Script stop = Script().await_interrupt().result(
      failed("none"), u(Status::kInterrupted));
  prepare(*w, 1, Script().message(Op::kSql, execute())
                     .result(failed("none"), u(Status::kInterrupted)));
  prepare(*w, 2, stop);
  // The kill comes while the statement runs, and the statement takes 3 G
  // to end.
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    hooks.kill = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * kGrace));
    r->kind = SqlResultKind::kOk;
  };
  auto t0 = Clock::now();
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted) << err;
  EXPECT_GE(ms_since(t0), 3 * kGrace);
  EXPECT_TRUE(w->usable()) << w->failure();
  hooks.kill = false;

  // The same with the clock already running: the statement calls a routine
  // of this worker, the interrupt is noticed while the host waits for that
  // nested RESULT (which counts), the nested routine stops at once, and
  // then the statement takes 3 G to end (which does not count).
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg nested;
    std::string e;
    EXPECT_EQ(w->call(2, 0, {}, hooks, &nested, &e), Outcome::kInterrupted);
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * kGrace));
    r->kind = SqlResultKind::kError;
    r->sql_errno = 1317;
    r->sqlstate = "70100";
    r->message = "Query execution was interrupted";
  };
  std::thread killer = kill_after(50);
  t0 = Clock::now();
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kInterrupted) << err;
  killer.join();
  EXPECT_GE(ms_since(t0), 50 + 3 * kGrace);
  EXPECT_TRUE(w->usable()) << w->failure();
  hooks.kill = false;

  // Waiting for a nested RESULT is not excepted: a nested routine that does
  // not stop is ended at G.
  prepare(*w, 3, Script().sleep(60000));
  hooks.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg nested;
    std::string e;
    const auto t1 = Clock::now();
    EXPECT_EQ(w->call(3, 0, {}, hooks, &nested, &e), Outcome::kKilled);
    EXPECT_LT(ms_since(t1), 50 + kGrace + 500);
    r->kind = SqlResultKind::kOk;
  };
  killer = kill_after(50);
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Outcome::kKilled);  // the nested outcome (HOST-2)
  killer.join();
  EXPECT_NE(err.find("did not stop within 300 ms"), std::string::npos) << err;
  EXPECT_FALSE(w->usable());
}

}  // namespace mle_worker_protocol_unittest
