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

// Tests of mle_launcher and the host side (Launcher, WorkerProcess) with a
// tiny test worker (mle_test_worker.cc), no GraalVM.
//
// The cgroup tests need a delegated cgroup v2 subtree; run for example
//   systemd-run --user --scope -p Delegate=yes ./mle_launcher-t
// Otherwise they are skipped.

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cgroup_test_util.h"
#include "escape_probes.h"
#include "mle/codec.h"
#include "mle/launcher.h"

namespace mle_launcher_unittest {

using namespace mle;
using mle_test::test_cgroup;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- helpers

std::string dir_of(const std::string &path) {
  return path.substr(0, path.rfind('/'));
}

ClassConfig test_class(const std::string &name,
                       std::vector<std::string> args = {"ready"}) {
  ClassConfig c;
  c.name = name;
  c.executable = MLE_TEST_WORKER_EXE;
  c.args = std::move(args);
  c.env = {"MLE_TEST=1", "LANG=C"};
  c.exec_paths = {dir_of(MLE_TEST_WORKER_EXE)};
  c.nofile = 256;
  return c;
}

LauncherConfig config(std::vector<ClassConfig> classes,
                      const std::string &cgroup = "") {
  LauncherConfig c;
  c.launcher_exe = MLE_LAUNCHER_EXE;
  c.cgroup_base = cgroup;
  c.arena_size = wire::kMinArenaSize;
  c.classes = std::move(classes);
  return c;
}

std::unique_ptr<Launcher> start(const LauncherConfig &c) {
  std::string error;
  auto l = Launcher::start(c, &error);
  EXPECT_NE(l, nullptr) << error;
  return l;
}

/** Waits for the worker's READY (the test worker posts it right away). */
bool wait_ready(const WorkerProcess &w, ReadyMsg *m, std::string *error,
                int timeout_ms = 10000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (w.header()->to_h.load(std::memory_order_acquire) == 0) {
    if (!w.alive())
      return *error = "worker died: " + w.exit_info(2000).describe(), false;
    if (Clock::now() > deadline) return *error = "no READY", false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  // Copy out, then check: the worker is untrusted.
  const wire::MessageHeader h = w.header()->msg;
  if (!check_worker_header(h, w.arena_size(), error)) return false;
  Assembler a(w.arena_size(), wire::kMaxMessageSize);
  if (a.add(h, w.arena()) != Assembler::Step::kDone)
    return *error = a.error(), false;
  if (h.op != static_cast<uint32_t>(wire::Op::kReady))
    return *error = "not READY", false;
  return decode(a.body(), m, error);
}

std::map<std::string, std::string> parse_report(const std::string &info) {
  std::map<std::string, std::string> r;
  std::istringstream in(info);
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos) r[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return r;
}

int count_open_fds() {
  int n = 0;
  if (DIR *d = opendir("/proc/self/fd")) {
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') n++;
    closedir(d);
  }
  return n - 1;  // the directory itself
}

/** Children of a process, from /proc/<pid>/task/<pid>/children. */
std::vector<pid_t> children_of(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/task/" +
                   std::to_string(pid) + "/children");
  std::vector<pid_t> r;
  pid_t c;
  while (in >> c) r.push_back(c);
  return r;
}

/** Waits until a process that is not our child has exited. */
bool wait_gone(pid_t pid, int timeout_ms) {
  const int fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
  if (fd < 0) return errno == ESRCH;
  pollfd p{fd, POLLIN, 0};
  const bool gone = poll(&p, 1, timeout_ms) > 0;
  close(fd);
  return gone;
}

ClassStats stats_of(Launcher &l, size_t index) {
  std::vector<ClassStats> s;
  std::string error;
  EXPECT_TRUE(l.stats(&s, &error)) << error;
  return s.size() > index ? s[index] : ClassStats{};
}

bool wait_idle(Launcher &l, size_t index, uint32_t idle, int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (stats_of(l, index).idle != idle) {
    if (Clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// ---------------------------------------------------------------- basics

TEST(MleLauncher, StartReportsCapabilities) {
  auto l = start(config({test_class("t")}));
  ASSERT_NE(l, nullptr);
  EXPECT_GT(l->info().pid, 0);
  EXPECT_GE(l->info().landlock_abi, launcher::kMinLandlockAbi)
      << "this kernel cannot isolate workers";
  ASSERT_EQ(l->info().class_errors.size(), 1u);
  EXPECT_EQ(l->info().class_errors[0], "");
  EXPECT_FALSE(l->info().cgroup_enabled);
}

TEST(MleLauncher, BadConfigurations) {
  std::string error;
  LauncherConfig bad = config({test_class("t")});
  bad.launcher_exe = "/nonexistent/mle_launcher";
  EXPECT_EQ(Launcher::start(bad, &error), nullptr);
  EXPECT_NE(error.find("nonexistent"), std::string::npos) << error;

  bad = config({test_class("t")});
  bad.arena_size = 1000;
  EXPECT_EQ(Launcher::start(bad, &error), nullptr);

  ClassConfig missing = test_class("missing");
  missing.executable = "/nonexistent/worker";
  ClassConfig relative = test_class("relative");
  relative.executable = "mle_test_worker";
  ClassConfig bad_path = test_class("bad_path");
  bad_path.read_paths = {"/nonexistent/venv"};
  auto l = start(config({missing, relative, bad_path, test_class("good")}));
  ASSERT_NE(l, nullptr);
  const auto &errors = l->info().class_errors;
  ASSERT_EQ(errors.size(), 4u);
  EXPECT_NE(errors[0].find("/nonexistent/worker"), std::string::npos);
  EXPECT_NE(errors[1].find("absolute"), std::string::npos);
  EXPECT_NE(errors[2].find("/nonexistent/venv"), std::string::npos);
  EXPECT_EQ(errors[3], "");
  EXPECT_EQ(l->acquire("missing", &error), nullptr);
  EXPECT_EQ(error, errors[0]);
  EXPECT_EQ(l->acquire("nosuchclass", &error), nullptr);
  EXPECT_NE(error.find("unknown"), std::string::npos);
  EXPECT_NE(l->acquire("good", &error), nullptr) << error;
}

TEST(MleLauncher, ExecFailureIsReported) {
  // Executable at CONFIG time, but not runnable under the lockdown: its
  // directory is not in the exec paths.
  ClassConfig c = test_class("t");
  c.exec_paths.clear();
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  std::string error;
  EXPECT_EQ(l->acquire("t", &error), nullptr);
  EXPECT_NE(error.find("exec: Permission denied"), std::string::npos) << error;
}

// The worker starts with exactly what the launcher gives it.
TEST(MleLauncher, WorkerEnvironment) {
  auto l = start(config({test_class("t")}));
  ASSERT_NE(l, nullptr);
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  EXPECT_EQ(ready.worker_class, "test");
  auto r = parse_report(ready.info);
  EXPECT_EQ(r["fds"], "0,1,2,3,4");
  EXPECT_EQ(r["cwd"], "/");
  EXPECT_EQ(r["env"], "MLE_TEST=1;LANG=C;");
  EXPECT_EQ(r["core"], "0");
  EXPECT_EQ(r["nofile"], "256");
  EXPECT_EQ(r["nnp"], "1");
  EXPECT_EQ(r["pdeathsig"], std::to_string(SIGKILL));
  EXPECT_EQ(r["seal"], "EPERM") << "the worker could resize the mailbox";
  EXPECT_EQ(r["stdin"], "/dev/null");
  EXPECT_EQ(r["ppid"], std::to_string(l->info().pid));
  EXPECT_EQ(w->header()->stats.tainted, 0u);
}

TEST(MleLauncher, ReleaseKillsTheWorker) {
  auto l = start(config({test_class("t", {"ignore_eof"})}));
  ASSERT_NE(l, nullptr);
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  const pid_t pid = w->pid();
  w.reset();
  EXPECT_TRUE(wait_gone(pid, 2000));
}

TEST(MleLauncher, ExitReports) {
  auto l = start(config({test_class("exit3", {"exit", "3"}),
                         test_class("segv", {"segv"}), test_class("ready")}));
  ASSERT_NE(l, nullptr);
  std::string error;

  auto w = l->acquire("exit3", &error);
  ASSERT_NE(w, nullptr) << error;
  ExitInfo e = w->exit_info(5000);
  ASSERT_TRUE(e.known);
  EXPECT_EQ(e.describe(), "exited with status 3");

  w = l->acquire("segv", &error);
  ASSERT_NE(w, nullptr) << error;
  e = w->exit_info(5000);
  ASSERT_TRUE(e.known);
  // ", core dumped" may follow: with a piped core_pattern the kernel sets
  // the flag although RLIMIT_CORE is 0 (the helper then stores nothing).
  EXPECT_EQ(e.describe().rfind("killed by signal 11 (Segmentation fault)", 0),
            0u)
      << e.describe();

  w = l->acquire("ready", &error);
  ASSERT_NE(w, nullptr) << error;
  EXPECT_TRUE(w->alive());
  EXPECT_FALSE(w->exit_info(50).known);  // still running
  EXPECT_TRUE(w->kill());
  e = w->exit_info(5000);
  ASSERT_TRUE(e.known);
  EXPECT_EQ(e.describe(), "killed by signal 9 (Killed)");
}

// ---------------------------------------------------------------- pool

TEST(MleLauncher, WarmPool) {
  ClassConfig c = test_class("t");
  c.pool_size = 2;
  auto l = start(config({c, test_class("cold")}));
  ASSERT_NE(l, nullptr);
  ASSERT_TRUE(wait_idle(*l, 0, 2, 5000));
  EXPECT_EQ(stats_of(*l, 1).idle, 0u);  // no pool for "cold"

  std::string error;
  std::vector<std::unique_ptr<WorkerProcess>> ws;
  for (int i = 0; i < 3; i++) {
    ws.push_back(l->acquire("t", &error));
    ASSERT_NE(ws.back(), nullptr) << error;
  }
  for (auto &w : ws) {
    ReadyMsg ready;
    ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  }
  ASSERT_TRUE(wait_idle(*l, 0, 2, 5000));  // refilled
  const ClassStats s = stats_of(*l, 0);
  EXPECT_EQ(s.handed_out, 3u);
  EXPECT_GE(s.spawned, 5u);
  EXPECT_EQ(s.pool_deaths, 0u);
  // Every worker is a different process.
  EXPECT_NE(ws[0]->pid(), ws[1]->pid());
  EXPECT_NE(ws[1]->pid(), ws[2]->pid());
}

TEST(MleLauncher, BrokenClassIsNotRefilledForever) {
  ClassConfig c = test_class("t", {"exit", "1"});  // dies in the pool
  c.pool_size = 2;
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  ClassStats s;
  while ((s = stats_of(*l, 0)).consecutive_pool_deaths <
             launcher::kMaxPoolDeaths &&
         Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GE(s.consecutive_pool_deaths, launcher::kMaxPoolDeaths);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  s = stats_of(*l, 0);
  // Workers already in the pool when refilling stops die too.
  EXPECT_LE(s.spawned, launcher::kMaxPoolDeaths + c.pool_size - 1)
      << "still refilling";
  EXPECT_FALSE(s.last_error.empty());
}

// No descriptors, processes or zombies left behind.
TEST(MleLauncher, ManyCyclesLeakNothing) {
  ClassConfig c = test_class("t");
  c.pool_size = 2;
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  ASSERT_TRUE(wait_idle(*l, 0, 2, 5000));
  const int fds_before = count_open_fds();
  const int cycles = std::getenv("MLE_TEST_CYCLES") != nullptr
                         ? atoi(std::getenv("MLE_TEST_CYCLES"))
                         : 1000;
  const auto t0 = Clock::now();
  for (int i = 0; i < cycles; i++) {
    std::string error;
    auto w = l->acquire("t", &error);
    ASSERT_NE(w, nullptr) << error << " at cycle " << i;
    ReadyMsg ready;
    ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error << " at cycle " << i;
  }
  const double ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::cout << "[ INFO     ] " << cycles
            << " acquire + READY + release: " << ms / cycles << " ms each\n";
  EXPECT_EQ(count_open_fds(), fds_before);
  ASSERT_TRUE(wait_idle(*l, 0, 2, 5000));
  // Released workers are reaped: the launcher has only its pool left.
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (children_of(l->info().pid).size() != 2 && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(children_of(l->info().pid).size(), 2u);
  EXPECT_EQ(stats_of(*l, 0).pool_deaths, 0u);
}

// mysqld calls acquire() from many session threads; exit reports must reach
// the right worker whichever thread happens to read them.
TEST(MleLauncher, ConcurrentSessions) {
  ClassConfig c = test_class("t");
  c.pool_size = 4;
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  constexpr int kThreads = 8, kPerThread = 50;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++)
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; i++) {
        std::string error;
        auto w = l->acquire("t", &error);
        ReadyMsg ready;
        if (w == nullptr || !wait_ready(*w, &ready, &error)) {
          failures++;
          continue;
        }
        if ((i + t) % 5 == 0) {  // some sessions see their worker die
          w->kill();
          const ExitInfo e = w->exit_info(5000);
          if (!e.known || e.describe() != "killed by signal 9 (Killed)")
            failures++;
        }
      }
    });
  for (auto &th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(stats_of(*l, 0).handed_out, uint64_t{kThreads * kPerThread});
}

// ---------------------------------------------------------------- death

TEST(MleLauncher, LauncherDeathKillsWorkers) {
  ClassConfig c = test_class("t");
  c.pool_size = 1;
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  ASSERT_TRUE(wait_idle(*l, 0, 1, 5000));
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  ASSERT_TRUE(wait_idle(*l, 0, 1, 5000));
  const std::vector<pid_t> pool = children_of(l->info().pid);
  kill(l->info().pid, SIGKILL);
  EXPECT_TRUE(w->wait_exit(3000)) << "handed-out worker survived";
  for (pid_t p : pool) EXPECT_TRUE(wait_gone(p, 3000)) << "pool worker " << p;
  // The host side copes with a dead launcher.
  EXPECT_EQ(l->acquire("t", &error), nullptr);
  EXPECT_FALSE(error.empty());
}

TEST(MleLauncher, HostDeathEndsLauncherAndWorkers) {
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);
  const pid_t host = fork();
  ASSERT_GE(host, 0);
  if (host == 0) {
    // A host that dies without cleaning up.
    ClassConfig c = test_class("t");
    c.pool_size = 1;
    std::string error;
    auto l = Launcher::start(config({c}), &error);
    if (!l) _exit(1);
    auto w = l->acquire("t", &error);
    if (!w) _exit(2);
    pid_t pids[2] = {l->info().pid, w->pid()};
    if (write(pipefd[1], pids, sizeof pids) != sizeof pids) _exit(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    _exit(0);
  }
  close(pipefd[1]);
  pid_t pids[2] = {0, 0};
  ASSERT_EQ(read(pipefd[0], pids, sizeof pids), ssize_t(sizeof pids));
  close(pipefd[0]);
  int status;
  waitpid(host, &status, 0);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  EXPECT_TRUE(wait_gone(pids[0], 3000)) << "launcher survived its host";
  EXPECT_TRUE(wait_gone(pids[1], 3000)) << "worker survived its host";
}

// ---------------------------------------------------------------- lockdown

class AbstractListener {
 public:
  explicit AbstractListener(const std::string &name) {
    m_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path + 1, name.data(), name.size());
    bind(m_fd, reinterpret_cast<sockaddr *>(&sa),
         static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 +
                                name.size()));
    listen(m_fd, 8);
  }
  ~AbstractListener() { close(m_fd); }

 private:
  int m_fd;
};

TEST(MleLauncher, LockdownDeniesEscapes) {
  char tmpl[] = "/tmp/mle-escape-XXXXXX";
  ASSERT_NE(mkdtemp(tmpl), nullptr);
  const std::string scratch = tmpl;
  const std::string secret = scratch + "/datafile.ibd";
  std::ofstream(secret) << "secret";
  const std::string allowed_dir = scratch + "-allowed";
  ASSERT_EQ(mkdir(allowed_dir.c_str(), 0700), 0);
  const std::string allowed = allowed_dir + "/lib.txt";
  std::ofstream(allowed) << "library";
  const std::string abstract_name = "mle-test-" + std::to_string(getpid());
  AbstractListener listener(abstract_name);

  mle_test::ProbeArgs a{getpid(), secret, scratch, allowed, 1, abstract_name};
  // Control: without the lockdown every probe works, so they are real.
  const auto open_access = mle_test::run_escape_probes(a);
  for (const auto &[probe, result] : open_access)
    EXPECT_EQ(result, "OK") << "control probe " << probe;
  std::ofstream(secret) << "secret";  // truncated by the control run

  ClassConfig c =
      test_class("t", {"escape", std::to_string(a.host_pid), a.secret_file,
                       a.scratch_dir, a.allowed_file,
                       std::to_string(a.closed_tcp_port), a.abstract_name});
  c.read_paths = {allowed_dir};
  auto l = start(config({c}));
  ASSERT_NE(l, nullptr);
  const int abi = l->info().landlock_abi;
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  auto r = parse_report(ready.info);
  EXPECT_EQ(r["read_allowed"], "OK");
  for (const char *probe :
       {"read_secret", "truncate_secret", "read_passwd", "create_file",
        "write_allowed", "host_environ", "exec_program"})
    EXPECT_EQ(r[probe], "EACCES") << probe;
  // These rely on newer Landlock ABIs; the worker's seccomp filter (W3)
  // denies them independently.
  if (abi >= 4) {
    EXPECT_EQ(r["tcp_connect"], "EACCES");
  }
  if (abi >= 6) {
    EXPECT_EQ(r["signal_host"], "EPERM");
    EXPECT_EQ(r["abstract_unix"], "EPERM");
  }
  std::ifstream in(secret);
  std::string content;
  in >> content;
  EXPECT_EQ(content, "secret");

  w.reset();
  unlink(secret.c_str());
  unlink((scratch + "/created").c_str());
  unlink(allowed.c_str());
  rmdir(scratch.c_str());
  rmdir(allowed_dir.c_str());
}

// ---------------------------------------------------------------- cgroup

int count_leaves(const std::string &base) {
  int n = 0;
  if (DIR *d = opendir(base.c_str())) {
    while (dirent *e = readdir(d))
      if (strncmp(e->d_name, "mle-", 4) == 0) n++;
    closedir(d);
  }
  return n;
}

TEST(MleLauncherCgroup, WorkersRunInTheirOwnLeaf) {
  NEED_CGROUP(base);
  auto l = start(config({test_class("t")}, base));
  ASSERT_NE(l, nullptr);
  ASSERT_TRUE(l->info().cgroup_enabled) << l->info().cgroup_info;
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  auto r = parse_report(ready.info);
  EXPECT_NE(r["cgroup"].find("/mle-" + std::to_string(l->info().pid) + "-w" +
                            std::to_string(w->id())),
            std::string::npos)
      << r["cgroup"];
  EXPECT_EQ(count_leaves(base), 1);
  w.reset();
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (count_leaves(base) != 0 && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(count_leaves(base), 0) << "leaf not removed";
}

TEST(MleLauncherCgroup, LaunchersShareABase) {
  // Several mysqld whose cgroups have one parent use one base. A leaf of a
  // launcher that is gone is removed at start; the others' are not touched.
  NEED_CGROUP(base);
  const std::string stale = base + "/mle-999999999-w1";  // > pid_max
  ASSERT_EQ(mkdir(stale.c_str(), 0755), 0) << strerror(errno);
  auto l1 = start(config({test_class("t")}, base));
  auto l2 = start(config({test_class("t")}, base));
  ASSERT_NE(l1, nullptr);
  ASSERT_NE(l2, nullptr);
  ASSERT_TRUE(l2->info().cgroup_enabled) << l2->info().cgroup_info;
  EXPECT_NE(access(stale.c_str(), F_OK), 0) << "stale leaf not removed";
  std::string error;
  auto w1 = l1->acquire("t", &error);
  ASSERT_NE(w1, nullptr) << error;
  auto w2 = l2->acquire("t", &error);
  ASSERT_NE(w2, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w1, &ready, &error)) << error;
  const std::string cgroup1 = parse_report(ready.info)["cgroup"];
  ASSERT_TRUE(wait_ready(*w2, &ready, &error)) << error;
  const std::string cgroup2 = parse_report(ready.info)["cgroup"];
  EXPECT_EQ(w1->id(), w2->id());  // the same worker id in each launcher
  EXPECT_NE(cgroup1, cgroup2);
  EXPECT_EQ(count_leaves(base), 2);
  w1.reset();
  w2.reset();
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (count_leaves(base) != 0 && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(count_leaves(base), 0);
}

TEST(MleLauncherCgroup, MemoryLimitEndsOnlyTheWorker) {
  NEED_CGROUP(base);
  ClassConfig c = test_class("t", {"oom"});
  c.memory_max = 64 << 20;
  auto l = start(config({c}, base));
  ASSERT_NE(l, nullptr);
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  const ExitInfo e = w->exit_info(10000);
  ASSERT_TRUE(e.known);
  EXPECT_GE(e.oom_kills, 1u);
  EXPECT_EQ(e.describe(),
            "killed by the out-of-memory killer (cgroup memory limit)");
}

TEST(MleLauncherCgroup, ProcessLimitAndLeftoverChildren) {
  NEED_CGROUP(base);
  ClassConfig c = test_class("t", {"fork", "20"});
  c.pids_max = 5;
  auto l = start(config({c}, base));
  ASSERT_NE(l, nullptr);
  std::string error;
  auto w = l->acquire("t", &error);
  ASSERT_NE(w, nullptr) << error;
  ReadyMsg ready;
  ASSERT_TRUE(wait_ready(*w, &ready, &error)) << error;
  EXPECT_EQ(ready.info, "forked=4");  // the worker itself is the fifth
  // The children have no parent-death signal; cgroup.kill ends them.
  w.reset();
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (count_leaves(base) != 0 && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(count_leaves(base), 0) << "children of the worker survived";
}

TEST(MleLauncherCgroup, UnusableBaseFallsBackWithReason) {
  ClassConfig c = test_class("t");
  auto l = start(config({c}, "/tmp"));
  ASSERT_NE(l, nullptr);
  EXPECT_FALSE(l->info().cgroup_enabled);
  EXPECT_NE(l->info().cgroup_info.find("not a cgroup v2"), std::string::npos)
      << l->info().cgroup_info;
  std::string error;
  EXPECT_NE(l->acquire("t", &error), nullptr) << error;
}

}  // namespace mle_launcher_unittest
