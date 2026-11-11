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

// W6: failures and escape attempts across Java, JS and Python in the JVM
// worker, under the full lockdown, through mle::Worker. Whatever a routine
// does, the host gets a clear status and keeps working; a worker that may be
// damaged is not used again.
//
// The memory-limit test needs a delegated cgroup:
//   systemd-run --user --scope -p Delegate=yes ./mle_worker_failures-t
// numpy tests need MLE_TEST_PYTHON_VENV.

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cgroup_test_util.h"
#include "jvm_test_util.h"

namespace mle_worker_failures_unittest {

using namespace mle;
using namespace mle_test;
using Clock = std::chrono::steady_clock;

#define NEED_NUMPY()                                                     \
  if (python_venv() == nullptr) {                                        \
    GTEST_SKIP() << "no GraalPy venv with numpy (MLE_TEST_PYTHON_VENV)"; \
  }                                                                      \
  static_assert(true)

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/** Starts a worker of cls and waits for READY. */
std::unique_ptr<TestHost> start_worker(Launcher &l, const std::string &cls,
                                       WorkerOptions o = {}) {
  std::string err;
  auto p = l.acquire(cls, &err);
  EXPECT_NE(p, nullptr) << err;
  if (!p) return nullptr;
  auto h = std::make_unique<TestHost>(std::move(p), o);
  ReadyMsg ready;
  EXPECT_TRUE(h->wait_ready(&ready, &err)) << err;
  return h;
}

/** Sets hooks.kill after delay_ms, from another thread. */
std::thread kill_after(TestHost &h, int delay_ms) {
  return std::thread([&h, delay_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    h.hooks.kill = true;
  });
}

// ---------------------------------------------------------------- cgroup
// First suite of the file: it needs the test process alone in its cgroup,
// before any launcher of the other suites exists.

TEST(MleWorkerMemoryLimit, NumpyBeyondTheCgroupLimit) {
  NEED_NUMPY();
  NEED_CGROUP(base);
  ClassConfig c = jvm_class("limited", "java,js,python");
  c.memory_max =
      2ULL << 30;  // JVM + GraalPy + numpy need > 1 GiB (page cache counts)
  LauncherConfig lc;
  lc.launcher_exe = MLE_LAUNCHER_EXE;
  lc.arena_size = wire::kMinArenaSize;
  lc.cgroup_base = base;
  lc.classes = {c};
  std::string err;
  auto l = Launcher::start(lc, &err);
  ASSERT_NE(l, nullptr) << err;
  ASSERT_TRUE(l->info().cgroup_enabled) << l->info().cgroup_info;
  auto h = start_worker(*l, "limited");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  // numpy memory is native: outside the Java heap, inside the cgroup.
  ASSERT_EQ(prepare_routine(*h, 1, "python", "f",
                            "import numpy as np\n"
                            "def f():\n"
                            "    keep = []\n"
                            "    while True:\n"
                            "        keep.append(np.ones(10**7))\n",
                            {}, type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kDied));
  EXPECT_EQ(error_of(res),
            "the MLE worker died: killed by the out-of-memory killer (cgroup "
            "memory limit)");
  std::cout << "[ INFO     ] killed by the cgroup OOM killer after "
            << ms_since(t0) << " ms\n";
  // The next worker of the class is fine.
  auto next = start_worker(*l, "limited");
  ASSERT_NE(next, nullptr);
  ASSERT_EQ(prepare_routine(*next, 1, "python", "f", "def f():\n    return 1\n",
                            {}, type_of(Tag::kI64), &res),
            0u);
  EXPECT_EQ(next->call(1, {}, &res), 0u) << error_of(res);
}

// ---------------------------------------------------------------- fixture

class MleWorkerFailures : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ClassConfig bad_option = jvm_class("bad-option", "");
    bad_option.args.push_back("-XX:+NoSuchMleOption");
    ClassConfig no_jar = jvm_class("no-jar", "");
    for (std::string &a : no_jar.args)
      if (a.rfind("-Djava.class.path=", 0) == 0)
        a = "-Djava.class.path=/nonexistent/mle-worker.jar";
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = wire::kMinArenaSize;
    c.classes = {jvm_class("poly", "java,js,python"),
                 jvm_class("small", "java,js,python", "256m"), bad_option,
                 no_jar};
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
    s_host = start_worker(*s_launcher, "poly").release();
    ASSERT_NE(s_host, nullptr);
  }
  static void TearDownTestSuite() {
    delete s_host;
    delete s_launcher;
  }

  static Launcher *s_launcher;
  static TestHost *s_host;
};

Launcher *MleWorkerFailures::s_launcher = nullptr;
TestHost *MleWorkerFailures::s_host = nullptr;

// ---------------------------------------------------------------- start

TEST_F(MleWorkerFailures, WorkersThatCannotStart) {
  for (const auto &[cls, why] :
       std::vector<std::pair<std::string, std::string>>{
           {"bad-option", "exited with status 24"},  // JNI_CreateJavaVM
           {"no-jar", "exited with status 25"}}) {   // Main not found
    SCOPED_TRACE(cls);
    std::string err;
    auto p = s_launcher->acquire(cls, &err);
    ASSERT_NE(p, nullptr) << err;
    Worker w(std::move(p));
    ReadyMsg ready;
    EXPECT_EQ(w.wait_ready(30000, &ready, &err), Status::kDied);
    EXPECT_EQ(err, "the MLE worker died: " + why);
    EXPECT_FALSE(w.usable());
  }
}

// ---------------------------------------------------------------- KILL

// Module-level Python code and top-level JS code run at PREPARE.
TEST_F(MleWorkerFailures, KillDuringPrepare) {
  auto h = start_worker(*s_launcher, "poly");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  const std::vector<std::pair<std::string, std::string>> loops = {
      {"python", "while True:\n    pass\ndef f():\n    return 1\n"},
      {"js", "for (;;) {}\nfunction f() { return 1; }"}};
  // The runtimes start first (GraalPy starts lazily, at the first Python
  // routine): the KILL must land in the routine's code, not in the start of
  // a language runtime (which may not stop within the grace period on a
  // loaded machine; the host then replaces the worker).
  ASSERT_EQ(prepare_routine(*h, 90, "python", "f", "def f():\n    return 0\n", {},
                            type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(prepare_routine(*h, 91, "js", "f", "function f() { return 0; }", {},
                            type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  uint64_t id = 1;
  for (const auto &[lang, src] : loops) {
    SCOPED_TRACE(lang);
    std::thread killer = kill_after(*h, 200);
    const auto t0 = Clock::now();
    EXPECT_EQ(
        prepare_routine(*h, id++, lang, "f", src, {}, type_of(Tag::kI64), &res),
        uint32_t(Status::kInterrupted))
        << error_of(res);
    killer.join();
    h->hooks.kill = false;
    std::cout << "[ INFO     ] " << lang << " PREPARE stopped "
              << ms_since(t0) - 200 << " ms after KILL\n";
    EXPECT_TRUE(h->worker.usable());
  }
  ASSERT_EQ(prepare_routine(*h, 10, "python", "f", "def f():\n    return 3\n",
                            {}, type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  EXPECT_EQ(h->call(10, {}, &res), 0u) << error_of(res);
}

// The session is killed while the routine's SQL runs on the host.
TEST_F(MleWorkerFailures, KillDuringSql) {
  auto h = start_worker(*s_launcher, "poly");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare_routine(*h, 1, "java", "Q.f",
                            "public class Q { public static long f() { long "
                            "n = 0; while (!Thread.currentThread()"
                            ".isInterrupted()) { com.percona.mle.api.Mle"
                            ".session().runSql(\"SELECT 1\"); n++; } return n; "
                            "} }",
                            {}, type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(prepare_routine(*h, 2, "js", "f",
                            "function f() { for (;;) session.runSql('SELECT 1'); }",
                            {}, type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  for (uint64_t id : {1, 2}) {
    SCOPED_TRACE(id == 1 ? "java" : "js");
    int statements = 0;
    auto sql = [&](const SqlMsg &, SqlResultMsg *r) {
      // KILL arrives while the 5th statement runs.
      if (++statements == 5) h->hooks.kill = true;
      r->kind = SqlResultKind::kOk;
    };
    EXPECT_EQ(h->call(id, {}, &res, sql), uint32_t(Status::kInterrupted))
        << error_of(res);
    EXPECT_LT(statements, 1000) << "the routine ran on after KILL";
    h->hooks.kill = false;
    EXPECT_TRUE(h->worker.usable());
  }
}

// A long computation in native numpy code cannot be interrupted at a guest
// safepoint; it ends by INTERRUPT or SIGKILL within the grace period. (On
// the JVM, an interrupt that reaches numpy's callback into GraalPy ends the
// JVM through FFM's uncaught-exception rule, U-5: reported as kKilled.)
TEST_F(MleWorkerFailures, KillLongNumpyComputation) {
  NEED_NUMPY();
  WorkerOptions o;
  o.interrupt_grace_ms = 500;
  auto h = start_worker(*s_launcher, "poly", o);
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare_routine(*h, 1, "python", "f",
                            "import numpy as np\n"
                            "def f():\n"
                            "    return float(np.linalg.svd(np.random.rand("
                            "5000, 5000))[1][0])\n",
                            {}, type_of(Tag::kF64), &res),
            0u)
      << error_of(res);
  std::thread killer = kill_after(*h, 300);
  const auto t0 = Clock::now();
  const uint32_t st = h->call(1, {}, &res);
  killer.join();
  const double ms = ms_since(t0) - 300;
  std::cout << "[ INFO     ] numpy SVD after KILL: status " << st << " ("
            << error_of(res) << ") after " << ms << " ms\n";
  EXPECT_TRUE(st == uint32_t(Status::kInterrupted) ||
              st == uint32_t(Status::kKilled))
      << st;
  EXPECT_LT(ms, 500 + 1000);
}

// ---------------------------------------------------------------- limits

TEST_F(MleWorkerFailures, GuestStackOverflowIsAGuestError) {
  ResultMsg res;
  const std::vector<std::tuple<std::string, std::string, std::string>> cases = {
      // JS stops at Oracle's frame limit (FrameLimit) before the stack ends.
      {"js", "function f() { return f() + 1; }",
       "Maximum frame limit of 1000 exceeded. Frames on stack: 1001."},
      {"python", "def f():\n    return f() + 1\n", "RecursionError"}};
  uint64_t id = 100;
  for (const auto &[lang, src, error] : cases) {
    SCOPED_TRACE(lang);
    ASSERT_EQ(prepare_routine(*s_host, ++id, lang, "f", src, {},
                              type_of(Tag::kI64), &res),
              0u);
    EXPECT_EQ(s_host->call(id, {}, &res), uint32_t(Status::kGuestError));
    EXPECT_NE(error_of(res).find(error), std::string::npos) << error_of(res);
    // Recoverable in the guest: the worker stays.
    EXPECT_TRUE(s_host->worker.usable());
  }
}

TEST_F(MleWorkerFailures, GuestHeapExhaustionRetiresTheWorker) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"js",
       "function f() { const keep = []; for (;;) keep.push(new "
       "Array(1000000).fill(1.5)); }"},
      {"python",
       "def f():\n    keep = []\n    while True:\n"
       "        keep.append([1.5] * 1000000)\n"}};
  for (const auto &[lang, src] : cases) {
    SCOPED_TRACE(lang);
    auto h = start_worker(*s_launcher, "small");
    ASSERT_NE(h, nullptr);
    ResultMsg res;
    ASSERT_EQ(
        prepare_routine(*h, 1, lang, "f", src, {}, type_of(Tag::kI64), &res),
        0u);
    const auto t0 = Clock::now();
    EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kExhausted))
        << error_of(res);
    std::cout << "[ INFO     ] " << lang << " heap exhausted after "
              << ms_since(t0) << " ms: " << error_of(res) << "\n";
    EXPECT_FALSE(h->worker.usable());
    EXPECT_TRUE(h->worker.process().wait_exit(2000));
  }
}

// ---------------------------------------------------------------- crashes

TEST_F(MleWorkerFailures, NativeCrashEndsOnlyTheWorker) {
  auto h = start_worker(*s_launcher, "poly");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare_routine(*h, 1, "python", "f",
                            "import ctypes\n"
                            "libc = ctypes.CDLL(None)\n"
                            "def f():\n"
                            "    libc.memset(ctypes.c_void_p(0), 0, 8)\n"
                            "    return 1\n",
                            {}, type_of(Tag::kI64), &res),
            0u)
      << error_of(res);
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kDied));
  std::cout << "[ INFO     ] " << error_of(res) << "\n";
  EXPECT_FALSE(h->worker.usable());
  // The host and the launcher are fine; the next worker works.
  auto next = start_worker(*s_launcher, "poly");
  ASSERT_NE(next, nullptr);
  ASSERT_EQ(prepare_routine(*next, 1, "js", "f", "function f() { return 2; }",
                            {}, type_of(Tag::kI64), &res),
            0u);
  EXPECT_EQ(next->call(1, {}, &res), 0u) << error_of(res);
}

// ---------------------------------------------------------------- escapes

// Native calls from Python (ctypes) bypass the JVM and Truffle entirely:
// only Landlock and seccomp stand in the way.
TEST_F(MleWorkerFailures, NativeEscapesFromPython) {
  ResultMsg res;
  ASSERT_EQ(
      prepare_routine(
          *s_host, 200, "python", "f",
          "import ctypes, errno\n"
          "libc = ctypes.CDLL(None, use_errno=True)\n"
          "def r(rc):\n"
          "    return 'OK' if rc >= 0 else "
          "errno.errorcode.get(ctypes.get_errno(), '?')\n"
          "def f(host):\n"
          "    out = []\n"
          "    out.append('open_passwd=' + r(libc.open(b'/etc/passwd', 0)))\n"
          "    out.append('create_tmp=' + r(libc.open(b'/tmp/mle-native', "
          "0o101, 0o600)))\n"
          "    out.append('host_mem=' + r(libc.open(('/proc/%d/mem' % "
          "host).encode(), 0)))\n"
          "    out.append('fork=' + r(libc.fork()))\n"
          "    out.append('socket=' + r(libc.socket(2, 1, 0)))\n"
          "    out.append('kill_host=' + r(libc.kill(host, 0)))\n"
          "    out.append('ptrace_host=' + r(libc.ptrace(16, host, 0, 0)))\n"
          "    return ' '.join(out)\n",
          {type_of(Tag::kI64)}, type_of(Tag::kString), &res),
      0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(200, {Value::i64(getpid())}, &res), 0u)
      << error_of(res);
  EXPECT_EQ(res.ret->as_text(),
            "open_passwd=EACCES create_tmp=EACCES host_mem=EACCES fork=EPERM "
            "socket=EPERM kill_host=EPERM ptrace_host=EPERM");
  EXPECT_EQ(access("/tmp/mle-native", F_OK), -1);
}

// JS reaches the JDK through host interop: the OS lockdown applies.
TEST_F(MleWorkerFailures, EscapesFromJsThroughJava) {
  ResultMsg res;
  ASSERT_EQ(
      prepare_routine(
          *s_host, 210, "js", "f",
          "function t(fn) { try { fn(); return 'OK'; } catch (e) { "
          "return e.getClass ? e.getClass().getSimpleName() : e.name; } }\n"
          "function f(host) {\n"
          "  const Files = Java.type('java.nio.file.Files');\n"
          "  const Path = Java.type('java.nio.file.Path');\n"
          "  return ['read=' + t(() => "
          "Files.readString(Path.of('/etc/passwd'))),\n"
          "          'write=' + t(() => "
          "Files.writeString(Path.of('/tmp/mle-js'), 'x')),\n"
          "          'exec=' + t(() => new "
          "(Java.type('java.lang.ProcessBuilder'))"
          "('/bin/true').start()),\n"
          "          'see_host=' + "
          "Java.type('java.lang.ProcessHandle').of(host)"
          ".isPresent()].join(' ');\n"
          "}",
          {type_of(Tag::kI64)}, type_of(Tag::kString), &res),
      0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(210, {Value::i64(getpid())}, &res), 0u)
      << error_of(res);
  EXPECT_EQ(res.ret->as_text(),
            "read=AccessDeniedException write=AccessDeniedException "
            "exec=IOException see_host=false");
  EXPECT_EQ(access("/tmp/mle-js", F_OK), -1);
}

}  // namespace mle_worker_failures_unittest
