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

// The host's Worker against a misbehaving worker (mle_test_worker "proto"
// modes, no GraalVM): every violation must end the worker, give the caller
// a clear status, and leave the host intact.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_worker_protocol_unittest {

using namespace mle;
using Clock = std::chrono::steady_clock;

std::string dir_of(const std::string &path) {
  return path.substr(0, path.rfind('/'));
}

class Hooks : public CallHooks {
 public:
  void sql(const SqlMsg &, SqlResultMsg *r) override {
    r->kind = SqlResultKind::kOk;
  }
  bool killed() override { return kill.load(); }
  std::atomic<bool> kill{false};
};

class MleWorkerProtocol : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = wire::kMinArenaSize;
    for (const char *how :
         {"good", "bad_op", "big_len", "bad_body", "wrong_depth", "wrong_seq",
          "status_without_error", "missing_return", "host_status", "chunk_lie",
          "die", "silent", "bad_stats", "tainted", "sql_in_prepare",
          "resize_mailbox", "out_for_in"}) {
      ClassConfig k;
      k.name = how;
      k.executable = MLE_TEST_WORKER_EXE;
      k.args = {"proto", how};
      k.exec_paths = {dir_of(MLE_TEST_WORKER_EXE)};
      c.classes.push_back(k);
    }
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
  }
  static void TearDownTestSuite() { delete s_launcher; }

  /** A ready Worker of the class, with routine 1 (no parameters, returns a
      BIGINT) prepared. */
  std::unique_ptr<Worker> worker(const std::string &cls,
                                 WorkerOptions options = {}) {
    std::string err;
    auto p = s_launcher->acquire(cls, &err);
    EXPECT_NE(p, nullptr) << err;
    if (!p) return nullptr;
    auto w = std::make_unique<Worker>(std::move(p), options);
    ReadyMsg ready;
    EXPECT_EQ(w->wait_ready(10000, &ready, &err), Status::kOk) << err;
    if (cls != "sql_in_prepare") {
      PrepareMsg prep;
      prep.routine_id = 1;
      prep.language = "test";
      ParamType t;
      t.tag = Tag::kI64;
      prep.returns = t;
      ResultMsg res;
      EXPECT_EQ(w->prepare(prep, hooks, &res, &err), Status::kOk) << err;
    }
    return w;
  }

  /** Calls routine 1 on a misbehaving worker and checks that it is gone. */
  void expect_violation(const std::string &cls, const std::string &what) {
    auto w = worker(cls);
    ASSERT_NE(w, nullptr);
    ResultMsg res;
    std::string err;
    EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kProtocolError)
        << cls;
    EXPECT_NE(err.find(what), std::string::npos) << cls << ": " << err;
    EXPECT_FALSE(w->usable());
    EXPECT_TRUE(w->process().wait_exit(2000)) << cls << ": still running";
    // Nothing more is sent to it.
    EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kDied);
  }

  Hooks hooks;
  static Launcher *s_launcher;
};

Launcher *MleWorkerProtocol::s_launcher = nullptr;

TEST_F(MleWorkerProtocol, GoodWorker) {
  auto w = worker("good");
  ASSERT_NE(w, nullptr);
  for (int i = 0; i < 1000; i++) {
    ResultMsg res;
    std::string err;
    ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kOk) << err;
    ASSERT_EQ(*res.ret, Value::i64(42));
  }
  std::string err;
  EXPECT_EQ(w->drop(1, &err), Status::kOk) << err;
  ResultMsg res;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kInternal);
  EXPECT_TRUE(w->usable());  // a host-side precondition, not the worker
  EXPECT_EQ(w->reset(&err), Status::kOk) << err;
}

TEST_F(MleWorkerProtocol, Violations) {
  expect_violation("bad_op", "unknown op");
  expect_violation("big_len", "larger than the arena");
  expect_violation("bad_body", "bad RESULT");
  expect_violation("wrong_depth", "wrong call");
  expect_violation("wrong_seq", "wrong call");
  expect_violation("status_without_error", "status and error disagree");
  expect_violation("missing_return", "missing return value");
  expect_violation("host_status", "bad status");
  expect_violation("chunk_lie", "after the last byte");
  expect_violation("out_for_in", "bad OUT parameter");
}

TEST_F(MleWorkerProtocol, SqlOutsideACall) {
  auto w = worker("sql_in_prepare");
  ASSERT_NE(w, nullptr);
  PrepareMsg prep;
  prep.routine_id = 1;
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->prepare(prep, hooks, &res, &err), Status::kProtocolError);
  EXPECT_NE(err.find("unexpected SQL during PREPARE"), std::string::npos)
      << err;
  EXPECT_FALSE(w->usable());
}

TEST_F(MleWorkerProtocol, WorkerDiesDuringACall) {
  auto w = worker("die");
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kDied);
  EXPECT_EQ(err, "the MLE worker died: exited with status 7");
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kDied);
  EXPECT_EQ(err, "the MLE worker died: exited with status 7");
}

// A worker that ignores KILL: INTERRUPT, then SIGKILL after the grace.
TEST_F(MleWorkerProtocol, SilentWorkerIsKilled) {
  WorkerOptions o;
  o.interrupt_grace_ms = 200;
  auto w = worker("silent", o);
  ASSERT_NE(w, nullptr);
  std::thread killer([this] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hooks.kill = true;
  });
  const auto t0 = Clock::now();
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kKilled);
  killer.join();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
          .count();
  EXPECT_GE(ms, 300);
  EXPECT_LT(ms, 1000);
  EXPECT_NE(err.find("did not stop within 200 ms"), std::string::npos) << err;
  EXPECT_FALSE(w->usable());
  EXPECT_EQ(w->process().exit_info(2000).describe(),
            "killed by signal 9 (Killed)");
}

TEST_F(MleWorkerProtocol, KilledBeforeTheCall) {
  auto w = worker("good");
  ASSERT_NE(w, nullptr);
  hooks.kill = true;
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kInterrupted);
  EXPECT_TRUE(w->usable());
  hooks.kill = false;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kOk) << err;
}

TEST_F(MleWorkerProtocol, StatsAreClamped) {
  auto w = worker("bad_stats");
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  ASSERT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kOk) << err;
  const WorkerStats s = w->stats();
  EXPECT_EQ(s.heap_max, 100u);
  EXPECT_EQ(s.heap_used, 100u);
  EXPECT_EQ(s.state, 0u);
}

TEST_F(MleWorkerProtocol, TaintedWorkerIsRetired) {
  auto w = worker("tainted");
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kOk) << err;
  EXPECT_FALSE(w->usable());
  EXPECT_NE(w->failure().find("retired"), std::string::npos);
  EXPECT_TRUE(w->process().wait_exit(2000));
}

TEST_F(MleWorkerProtocol, MailboxCannotBeResized) {
  auto w = worker("resize_mailbox");
  ASSERT_NE(w, nullptr);
  ResultMsg res;
  std::string err;
  EXPECT_EQ(w->call(1, 0, {}, hooks, &res, &err), Status::kOk) << err;
  EXPECT_TRUE(w->usable());
}

TEST_F(MleWorkerProtocol, ReadyTimeout) {
  // "silent" answers READY; a worker that never posts READY is simulated by
  // waiting twice (the second READY never comes).
  auto w = worker("good");
  ASSERT_NE(w, nullptr);
  ReadyMsg ready;
  std::string err;
  EXPECT_EQ(w->wait_ready(100, &ready, &err), Status::kDied);
  EXPECT_NE(err.find("no READY"), std::string::npos) << err;
}

}  // namespace mle_worker_protocol_unittest
