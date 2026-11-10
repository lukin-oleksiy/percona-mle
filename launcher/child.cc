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

// The worker side of a spawn, between clone3 and exec, and the Landlock
// rules. The launcher is single-threaded, so the child may allocate.

#include <fcntl.h>
#include <linux/landlock.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <vector>

#include "launcher_internal.h"
#include "mle/protocol.h"

namespace mle::launcher {

namespace {

// Our own copies of the Landlock ABI structures and flags, so that the
// launcher builds with older kernel headers; the kernel ABI is stable.
struct RulesetAttr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;  // ABI 4
  uint64_t scoped;              // ABI 6
};
struct PathBeneathAttr {
  uint64_t allowed_access;
  int32_t parent_fd;
} __attribute__((packed));

constexpr uint64_t kFsExecute = 1ULL << 0;
constexpr uint64_t kFsWriteFile = 1ULL << 1;
constexpr uint64_t kFsReadFile = 1ULL << 2;
constexpr uint64_t kFsReadDir = 1ULL << 3;
constexpr uint64_t kFsAbi1 = (1ULL << 13) - 1;  // EXECUTE .. MAKE_SYM
constexpr uint64_t kFsRefer = 1ULL << 13;       // ABI 2
constexpr uint64_t kFsTruncate = 1ULL << 14;    // ABI 3
constexpr uint64_t kFsIoctlDev = 1ULL << 15;    // ABI 5
constexpr uint64_t kNetBindTcp = 1ULL << 0;
constexpr uint64_t kNetConnectTcp = 1ULL << 1;
constexpr uint64_t kScopeAbstractUnix = 1ULL << 0;
constexpr uint64_t kScopeSignal = 1ULL << 1;
constexpr int kRulePathBeneath = 1;
constexpr uint32_t kCreateRulesetVersion = 1U << 0;

// Rights that make sense on a file (not a directory).
constexpr uint64_t kFileRights =
    kFsExecute | kFsWriteFile | kFsReadFile | kFsTruncate | kFsIoctlDev;

constexpr uint64_t kRead = kFsReadFile | kFsReadDir;
constexpr uint64_t kReadExec = kRead | kFsExecute;

/** Read and execute: the dynamic loader and the system libraries. */
const char *const kSystemExecPaths[] = {"/usr/lib", "/lib", "/lib64",
                                        "/usr/lib64", "/usr/libexec"};
/** Read only: what glibc, the JVM and GraalVM read at start. */
const char *const kSystemReadPaths[] = {
    "/etc/ld.so.cache",        "/etc/localtime", "/usr/share/zoneinfo",
    "/sys/devices/system/cpu", "/proc/meminfo",  "/proc/stat",
    "/proc/cpuinfo",           "/dev/urandom",   "/dev/random"};

int add_rule(int ruleset, const char *path, uint64_t access, uint64_t handled) {
  const int fd = open(path, O_PATH | O_CLOEXEC);
  if (fd < 0) return 0;  // a missing system path is simply not allowed
  struct stat st;
  if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) access &= kFileRights;
  PathBeneathAttr pb{access & handled, fd};
  const long r =
      syscall(SYS_landlock_add_rule, ruleset, kRulePathBeneath, &pb, 0);
  const int err = errno;
  close(fd);
  return r == 0 ? 0 : err;
}

}  // namespace

int landlock_abi() {
  const long abi =
      syscall(SYS_landlock_create_ruleset, nullptr, 0, kCreateRulesetVersion);
  return abi < 0 ? 0 : static_cast<int>(abi);
}

int apply_landlock(int abi, const ClassConfig &c, const std::string &leaf) {
  RulesetAttr attr{};
  attr.handled_access_fs = kFsAbi1;
  if (abi >= 2) attr.handled_access_fs |= kFsRefer;
  if (abi >= 3) attr.handled_access_fs |= kFsTruncate;
  if (abi >= 5) attr.handled_access_fs |= kFsIoctlDev;
  size_t size = offsetof(RulesetAttr, handled_access_net);
  if (abi >= 4) {
    attr.handled_access_net = kNetBindTcp | kNetConnectTcp;
    size = offsetof(RulesetAttr, scoped);
  }
  if (abi >= 6) {
    attr.scoped = kScopeAbstractUnix | kScopeSignal;
    size = sizeof attr;
  }
  const uint64_t handled = attr.handled_access_fs;
  const int rs =
      static_cast<int>(syscall(SYS_landlock_create_ruleset, &attr, size, 0));
  if (rs < 0) return errno;

  int err = 0;
  auto allow = [&](const std::string &path, uint64_t access) {
    if (err == 0) err = add_rule(rs, path.c_str(), access, handled);
  };
  for (const char *p : kSystemExecPaths) allow(p, kReadExec);
  for (const char *p : kSystemReadPaths) allow(p, kRead);
  for (const std::string &p : c.exec_paths) allow(p, kReadExec);
  for (const std::string &p : c.read_paths) allow(p, kRead);
  allow("/proc/" + std::to_string(getpid()), kRead);
  if (!leaf.empty()) allow(leaf, kRead);  // the JVM reads its limits here
  allow("/dev/null", kFsReadFile | kFsWriteFile);

  if (err == 0 && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) err = errno;
  if (err == 0 && syscall(SYS_landlock_restrict_self, rs, 0) != 0) err = errno;
  close(rs);
  return err;
}

const char *child_step_name(int32_t step) {
  switch (step) {
    case kStepPdeathsig:
      return "parent death signal";
    case kStepFds:
      return "file descriptors";
    case kStepChdir:
      return "chdir";
    case kStepRlimit:
      return "resource limits";
    case kStepLandlock:
      return "Landlock";
    case kStepExec:
      return "exec";
  }
  return "?";
}

[[noreturn]] static void fail(int err_pipe, int32_t step, int err) {
  const ChildFailure f{step, err};
  [[maybe_unused]] ssize_t n = write(err_pipe, &f, sizeof f);
  _exit(127);
}

[[noreturn]] void child_main(const SpawnRequest &r) {
  const ClassConfig &c = *r.cls;
  int err_pipe = r.err_pipe;

  // Die with the launcher (it is single-threaded, so "parent thread" is the
  // launcher itself), and do not start if it is already gone.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
    fail(err_pipe, kStepPdeathsig, errno);
  if (getppid() != r.launcher_pid) fail(err_pipe, kStepPdeathsig, ESRCH);

  // Signals: dispositions set to SIG_IGN survive exec (the launcher ignores
  // SIGPIPE); reset everything and unblock everything.
  for (int sig = 1; sig < NSIG; sig++) signal(sig, SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);

  // Descriptors: 0, 1, 2 -> /dev/null (2 may stay inherited), 3 = mailbox,
  // 4 = control, 5 = the error pipe (close-on-exec), nothing else.
  const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
  const int null_hi = fcntl(devnull, F_DUPFD_CLOEXEC, 10);
  const int mailbox_hi = fcntl(r.mailbox, F_DUPFD_CLOEXEC, 10);
  const int control_hi = fcntl(r.control, F_DUPFD_CLOEXEC, 10);
  const int pipe_hi = fcntl(err_pipe, F_DUPFD_CLOEXEC, 10);
  if (devnull < 0 || null_hi < 0 || mailbox_hi < 0 || control_hi < 0 ||
      pipe_hi < 0)
    fail(err_pipe, kStepFds, errno);
  err_pipe = pipe_hi;
  if (dup2(null_hi, 0) < 0 || dup2(null_hi, 1) < 0 ||
      (!c.inherit_stderr && dup2(null_hi, 2) < 0) ||
      dup2(mailbox_hi, wire::kMailboxFd) < 0 ||
      dup2(control_hi, wire::kControlFd) < 0 || dup3(pipe_hi, 5, O_CLOEXEC) < 0)
    fail(err_pipe, kStepFds, errno);
  err_pipe = 5;
  if (syscall(SYS_close_range, 6U, ~0U, 0U) != 0)
    fail(err_pipe, kStepFds, errno);

  if (chdir("/") != 0) fail(err_pipe, kStepChdir, errno);

  rlimit lim{c.nofile, c.nofile};
  if (c.nofile > 0 && setrlimit(RLIMIT_NOFILE, &lim) != 0)
    fail(err_pipe, kStepRlimit, errno);
  if (!c.core_dumps) {
    rlimit zero{0, 0};
    if (setrlimit(RLIMIT_CORE, &zero) != 0) fail(err_pipe, kStepRlimit, errno);
  }

  if (r.landlock_abi >= kMinLandlockAbi) {
    if (const int err = apply_landlock(r.landlock_abi, c, r.leaf); err != 0)
      fail(err_pipe, kStepLandlock, err);
  } else if (!c.insecure_without_landlock) {
    fail(err_pipe, kStepLandlock, ENOSYS);  // checked at CONFIG already
  }

  std::vector<std::string> args{c.executable};
  args.insert(args.end(), c.args.begin(), c.args.end());
  std::vector<char *> argv, envp;
  for (std::string &a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::vector<std::string> env = c.env;
  for (std::string &e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  execve(c.executable.c_str(), argv.data(), envp.data());
  fail(err_pipe, kStepExec, errno);
}

}  // namespace mle::launcher
