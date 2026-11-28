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

#include "mle/launcher.h"

#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "mle/codec.h"

namespace mle {

namespace {

using Clock = std::chrono::steady_clock;
using launcher::Msg;

constexpr size_t kMaxDatagram = 256 * 1024;
constexpr int kLauncherExitGraceMs = 3000;

int pidfd_open(pid_t pid) {
  return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
}

bool pidfd_kill(int pidfd) {
  return syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0) == 0 ||
         errno == ESRCH;
}

/** True if the pidfd became readable (the process exited) within timeout. */
bool pidfd_wait(int pidfd, int timeout_ms) {
  pollfd p{pidfd, POLLIN, 0};
  int r;
  do {
    r = poll(&p, 1, timeout_ms);
  } while (r < 0 && errno == EINTR);
  return r > 0;
}

int remaining_ms(Clock::time_point deadline) {
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - Clock::now())
                  .count();
  return left < 0 ? 0 : static_cast<int>(left);
}

void close_all(const std::vector<int> &fds) {
  for (int fd : fds) close(fd);
}

std::string errno_text(const char *what) {
  return std::string(what) + ": " + strerror(errno);
}

}  // namespace

std::string ExitInfo::describe() const {
  if (!known) return "exit status unknown";
  if (WIFEXITED(status))
    return "exited with status " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    if (sig == SIGKILL && oom_kills > 0)
      return "killed by the out-of-memory killer (cgroup memory limit)";
    std::string s = "killed by signal " + std::to_string(sig);
    if (const char *name = sigdescr_np(sig))
      s += std::string(" (") + name + ")";
    if (WCOREDUMP(status)) s += ", core dumped";
    return s;
  }
  return "ended with wait status " + std::to_string(status);
}

// ---------------------------------------------------------------- channel

/** The socket to mle_launcher and everything that goes with it. Shared by
    the Launcher and the WorkerProcess objects it handed out. */
class LauncherChannel {
 public:
  ~LauncherChannel();

  bool start(const LauncherConfig &config, std::string *error);
  /** Sends a request and waits for its reply (request id matched). */
  bool request(Msg type, uint64_t id, const std::string &body, Msg reply_type,
               std::string *reply, std::vector<int> *fds, std::string *error);
  uint64_t next_id() { return ++m_last_id; }
  /** False once the launcher process has exited. */
  bool alive() const { return m_pidfd >= 0 && !pidfd_wait(m_pidfd, 0); }

  /** Caller holds mutex. */
  void track_locked(uint64_t worker_id) { m_live.insert(worker_id); }
  void forget(uint64_t worker_id);
  bool exit_report(uint64_t worker_id, int timeout_ms, ExitInfo *info);

  LauncherInfo info;
  std::vector<std::string> class_names;
  uint32_t arena_size = 0;
  int timeout_ms = 0;
  std::mutex mutex;

 private:
  enum class Recv { kMessage, kTimeout, kError };
  Recv receive(int timeout_ms, std::string *body, std::vector<int> *fds,
               std::string *error);
  /** Handles a message nobody waits for (kExited); false if unexpected. */
  bool handle_async(const std::string &body, const std::vector<int> &fds);

  int m_socket = -1;
  int m_pidfd = -1;
  pid_t m_pid = -1;
  uint64_t m_last_id = 0;
  std::set<uint64_t> m_live;
  std::map<uint64_t, ExitInfo> m_exits;
};

LauncherChannel::~LauncherChannel() {
  if (m_socket >= 0) close(m_socket);  // EOF: the launcher shuts down
  if (m_pidfd >= 0) {
    if (!pidfd_wait(m_pidfd, kLauncherExitGraceMs)) pidfd_kill(m_pidfd);
    siginfo_t si;
    // We are the launcher's parent, so we reap it. ECHILD (SIGCHLD ignored
    // by the host) is fine.
    while (waitid(P_PIDFD, static_cast<id_t>(m_pidfd), &si, WEXITED) < 0 &&
           errno == EINTR) {
    }
    close(m_pidfd);
  }
}

bool LauncherChannel::start(const LauncherConfig &config, std::string *error) {
  timeout_ms = config.timeout_ms;
  arena_size = config.arena_size;
  for (const ClassConfig &c : config.classes) class_names.push_back(c.name);

  if (access(config.launcher_exe.c_str(), X_OK) != 0) {
    *error = errno_text(("launcher " + config.launcher_exe).c_str());
    return false;
  }
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
    *error = errno_text("socketpair");
    return false;
  }
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  // The launcher gets fd 3 and nothing else of ours: mysqld has descriptors
  // without O_CLOEXEC.
  posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
  posix_spawn_file_actions_addclosefrom_np(&fa, 4);
  // The spawning thread may block signals (mysqld does); start clean.
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &all);
  // Its own session: signals for the host's process group or terminal
  // (Ctrl-C, a test framework killing the group) do not reach the launcher
  // and its workers; it ends when the host closes the socket, and cleans
  // up (kills the workers, removes their cgroups).
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK |
                                      POSIX_SPAWN_SETSIGDEF |
                                      POSIX_SPAWN_SETSID);
  std::string exe = config.launcher_exe;
  char *argv[] = {exe.data(), nullptr};
  // Diagnostics: the only variables the launcher gets (its workers' sandbox
  // allows writes in that directory; README, launcher/child.cc), and a
  // test's region version (honoured by debug builds of the launcher only).
  std::string debug_dir, region_version;
  if (!config.debug_write_dir.empty())
    debug_dir = "MLE_DEBUG_WRITE_DIR=" + config.debug_write_dir;
  if (config.test_region_version != 0)
    region_version =
        "MLE_TEST_REGION_VERSION=" + std::to_string(config.test_region_version);
  char *envp[3] = {nullptr, nullptr, nullptr};
  size_t nenv = 0;
  if (!debug_dir.empty()) envp[nenv++] = debug_dir.data();
  if (!region_version.empty()) envp[nenv++] = region_version.data();
  const int rc = posix_spawn(&m_pid, exe.c_str(), &fa, &attr, argv, envp);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(sv[1]);
  m_socket = sv[0];
  if (rc != 0) {
    *error = std::string("posix_spawn launcher: ") + strerror(rc);
    return false;
  }
  info.pid = m_pid;
  // Our unreaped child: its pid cannot be reused, so this is race-free.
  m_pidfd = pidfd_open(m_pid);
  if (m_pidfd < 0) {
    *error = errno_text("pidfd_open");
    kill(m_pid, SIGKILL);
    waitpid(m_pid, nullptr, 0);
    return false;
  }

  std::string body;
  Writer w(&body);
  w.u8(static_cast<uint8_t>(Msg::kConfig));
  w.u32(launcher::kVersion);
  w.u32(config.arena_size);
  w.str(config.cgroup_base);
  w.u32(static_cast<uint32_t>(config.classes.size()));
  for (const ClassConfig &c : config.classes) launcher::put(w, c);

  std::string reply;
  std::vector<int> fds;
  if (!request(Msg::kConfig, 0, body, Msg::kConfigReply, &reply, &fds, error))
    return false;
  close_all(fds);
  Reader r(reply);
  uint8_t type;
  bool ok, cgroup;
  std::string launcher_error;
  uint32_t abi;
  if (!r.u8(&type) || !r.flag(&ok) || !r.str(&launcher_error) || !r.u32(&abi) ||
      !r.flag(&cgroup) || !r.str(&info.cgroup_info) ||
      !launcher::get(r, &info.class_errors) || !r.finish()) {
    *error = "bad configuration reply from the launcher: " + r.error();
    return false;
  }
  if (!ok) {  // a refused configuration comes without its classes
    *error = "launcher: " + launcher_error;
    return false;
  }
  if (info.class_errors.size() != config.classes.size()) {
    *error = "bad configuration reply from the launcher: " +
             std::to_string(info.class_errors.size()) + " classes";
    return false;
  }
  info.landlock_abi = static_cast<int>(abi);
  info.cgroup_enabled = cgroup;
  return true;
}

LauncherChannel::Recv LauncherChannel::receive(int wait_ms, std::string *body,
                                               std::vector<int> *fds,
                                               std::string *error) {
  pollfd p{m_socket, POLLIN, 0};
  int r;
  do {
    r = poll(&p, 1, wait_ms);
  } while (r < 0 && errno == EINTR);
  if (r == 0) return Recv::kTimeout;
  if (r < 0) {
    *error = errno_text("poll launcher");
    return Recv::kError;
  }
  body->resize(kMaxDatagram);
  iovec iov{body->data(), body->size()};
  alignas(cmsghdr) char cbuf[CMSG_SPACE(4 * sizeof(int))];
  msghdr m{};
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = cbuf;
  m.msg_controllen = sizeof cbuf;
  ssize_t n;
  do {
    n = recvmsg(m_socket, &m, MSG_CMSG_CLOEXEC);
  } while (n < 0 && errno == EINTR);
  fds->clear();
  if (n < 0) {  // no control data was written: nothing to walk
    *error = errno_text("recvmsg launcher");
    return Recv::kError;
  }
  for (cmsghdr *c = CMSG_FIRSTHDR(&m); c != nullptr; c = CMSG_NXTHDR(&m, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    if (c->cmsg_len < CMSG_LEN(0)) break;
    const size_t count = std::min<size_t>((c->cmsg_len - CMSG_LEN(0)) / sizeof(int), 4);
    for (size_t i = 0; i < count; i++) {
      int fd;
      memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
      fds->push_back(fd);
    }
  }
  if (n <= 0) {
    close_all(*fds);
    fds->clear();
    *error = n == 0 ? "the launcher exited" : errno_text("recvmsg launcher");
    return Recv::kError;
  }
  if ((m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    close_all(*fds);
    fds->clear();
    *error = "truncated message from the launcher";
    return Recv::kError;
  }
  body->resize(static_cast<size_t>(n));
  return Recv::kMessage;
}

bool LauncherChannel::handle_async(const std::string &body,
                                   const std::vector<int> &fds) {
  Reader r(body);
  uint8_t type;
  uint64_t worker_id;
  uint32_t status;
  ExitInfo e;
  if (!fds.empty() || !r.u8(&type) ||
      type != static_cast<uint8_t>(Msg::kExited) || !r.u64(&worker_id) ||
      !r.u32(&status) || !r.u64(&e.oom_kills) || !r.finish())
    return false;
  e.known = true;
  e.status = static_cast<int>(status);
  // Reports of workers already released are dropped, so the map stays small.
  if (m_live.count(worker_id) != 0) m_exits[worker_id] = e;
  return true;
}

bool LauncherChannel::request(Msg type, uint64_t id, const std::string &body,
                              Msg reply_type, std::string *reply,
                              std::vector<int> *fds, std::string *error) {
  if (send(m_socket, body.data(), body.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(body.size())) {
    *error = errno_text("send to launcher");
    return false;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    switch (receive(remaining_ms(deadline), reply, fds, error)) {
      case Recv::kTimeout:
        *error = "no answer from the launcher";
        return false;
      case Recv::kError:
        return false;
      case Recv::kMessage:
        break;
    }
    if (handle_async(*reply, *fds)) continue;
    Reader r(*reply);
    uint8_t got = 0;
    uint64_t got_id = 0;
    const bool has_id =
        r.u8(&got) && (got == static_cast<uint8_t>(Msg::kWorker) ||
                       got == static_cast<uint8_t>(Msg::kStatsReply)) &&
        r.u64(&got_id);
    if (got == static_cast<uint8_t>(reply_type) &&
        (type == Msg::kConfig || (has_id && got_id == id)))
      return true;
    // The answer to a request that timed out earlier (ids only grow): drop
    // it and keep waiting for ours, or every later request would read its
    // predecessor's answer (independent review C19). A worker nobody waits for any
    // more is ended through its pidfd (the last of its three descriptors).
    const bool stale = type != Msg::kConfig && has_id && got_id < id;
    if (stale && got == static_cast<uint8_t>(Msg::kWorker) && fds->size() == 3)
      (void)pidfd_kill(fds->back());
    close_all(*fds);
    fds->clear();
    if (stale) continue;
    *error = "unexpected message from the launcher";
    return false;
  }
}

void LauncherChannel::forget(uint64_t worker_id) {
  std::lock_guard<std::mutex> lock(mutex);
  m_live.erase(worker_id);
  m_exits.erase(worker_id);
}

bool LauncherChannel::exit_report(uint64_t worker_id, int wait_ms,
                                  ExitInfo *out) {
  std::lock_guard<std::mutex> lock(mutex);
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  while (true) {
    auto it = m_exits.find(worker_id);
    if (it != m_exits.end()) {
      *out = it->second;
      return true;
    }
    std::string body, error;
    std::vector<int> fds;
    if (receive(remaining_ms(deadline), &body, &fds, &error) != Recv::kMessage)
      return false;
    if (!handle_async(body, fds)) close_all(fds);  // nothing else expected
  }
}

// ---------------------------------------------------------------- worker

WorkerProcess::~WorkerProcess() {
  if (m_pidfd >= 0) kill();
  if (m_header != nullptr) munmap(m_header, wire::region_size(m_arena_size));
  for (int fd : {m_mailbox, m_control, m_pidfd})
    if (fd >= 0) close(fd);
  if (m_channel) m_channel->forget(m_id);
}

bool WorkerProcess::alive() const { return !pidfd_wait(m_pidfd, 0); }

bool WorkerProcess::kill() const { return pidfd_kill(m_pidfd); }

bool WorkerProcess::wait_exit(int timeout_ms) const {
  return pidfd_wait(m_pidfd, timeout_ms);
}

ExitInfo WorkerProcess::exit_info(int timeout_ms) const {
  ExitInfo e;
  const auto start = Clock::now();
  if (!wait_exit(timeout_ms)) return e;
  const int left = remaining_ms(start + std::chrono::milliseconds(timeout_ms));
  if (!m_channel->exit_report(m_id, left, &e)) e.known = false;
  return e;
}

// ---------------------------------------------------------------- launcher

std::unique_ptr<Launcher> Launcher::start(const LauncherConfig &config,
                                          std::string *error) {
  if (!wire::is_valid_arena_size(config.arena_size)) {
    *error = "arena size must be a multiple of 4096, at least " +
             std::to_string(wire::kMinArenaSize) + " and at most " +
             std::to_string(wire::kMaxArenaSize);
    return nullptr;
  }
  auto channel = std::make_shared<LauncherChannel>();
  if (!channel->start(config, error)) return nullptr;
  std::unique_ptr<Launcher> l(new Launcher());
  l->m_channel = std::move(channel);
  return l;
}

Launcher::~Launcher() = default;

const LauncherInfo &Launcher::info() const { return m_channel->info; }

bool Launcher::alive() const { return m_channel->alive(); }

std::unique_ptr<WorkerProcess> Launcher::acquire(const std::string &cls,
                                                 std::string *error) {
  LauncherChannel &ch = *m_channel;
  uint32_t index = 0;
  while (index < ch.class_names.size() && ch.class_names[index] != cls) index++;
  if (index == ch.class_names.size()) {
    *error = "unknown worker class '" + cls + "'";
    return nullptr;
  }

  std::string reply;
  std::vector<int> fds;
  uint64_t worker_id;
  uint32_t pid;
  {
    // Held until the worker is tracked, so that its exit report (read by
    // whichever thread receives next) is never dropped as unknown.
    std::lock_guard<std::mutex> lock(ch.mutex);
    const uint64_t id = ch.next_id();
    std::string body;
    Writer w(&body);
    w.u8(static_cast<uint8_t>(Msg::kAcquire));
    w.u64(id);
    w.u32(index);
    if (!ch.request(Msg::kAcquire, id, body, Msg::kWorker, &reply, &fds, error))
      return nullptr;
    Reader r(reply);
    uint8_t type;
    uint64_t request_id;
    bool ok;
    std::string launcher_error;
    if (!r.u8(&type) || !r.u64(&request_id) || !r.flag(&ok) ||
        !r.str(&launcher_error) || !r.u64(&worker_id) || !r.u32(&pid) ||
        !r.finish()) {
      close_all(fds);
      *error = "bad worker message from the launcher: " + r.error();
      return nullptr;
    }
    if (!ok) {
      close_all(fds);
      *error = launcher_error;
      return nullptr;
    }
    if (fds.size() != 3) {
      close_all(fds);
      *error = "launcher sent " + std::to_string(fds.size()) +
               " descriptors instead of 3";
      return nullptr;
    }
    ch.track_locked(worker_id);
  }

  std::unique_ptr<WorkerProcess> w(new WorkerProcess());
  w->m_channel = m_channel;
  w->m_class = cls;
  w->m_id = worker_id;
  w->m_pid = static_cast<pid_t>(pid);
  w->m_mailbox = fds[0];
  w->m_control = fds[1];
  w->m_pidfd = fds[2];
  w->m_arena_size = ch.arena_size;

  // The region must have the size we configured and all three seals: then
  // no access to the mapping can fault, whatever the worker does (§2).
  const size_t size = wire::region_size(ch.arena_size);
  constexpr int kSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
  struct stat st;
  const int seals = fcntl(w->m_mailbox, F_GET_SEALS);
  if (fstat(w->m_mailbox, &st) != 0 ||
      static_cast<size_t>(st.st_size) != size || seals < 0 ||
      (seals & kSeals) != kSeals) {
    *error = "worker mailbox has the wrong size or is not sealed";
    return nullptr;
  }
  void *p =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, w->m_mailbox, 0);
  if (p == MAP_FAILED) {
    *error = errno_text("mmap worker mailbox");
    return nullptr;
  }
  // Line 0 of the header is for the worker; the host uses what it
  // configured and does not read it.
  w->m_header = static_cast<wire::RegionHeader *>(p);
  return w;
}

bool Launcher::stats(std::vector<ClassStats> *stats, std::string *error) {
  LauncherChannel &ch = *m_channel;
  std::string reply;
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(ch.mutex);
    const uint64_t id = ch.next_id();
    std::string body;
    Writer w(&body);
    w.u8(static_cast<uint8_t>(Msg::kStats));
    w.u64(id);
    if (!ch.request(Msg::kStats, id, body, Msg::kStatsReply, &reply, &fds,
                    error))
      return false;
  }
  close_all(fds);
  Reader r(reply);
  uint8_t type;
  uint64_t id;
  uint32_t n;
  if (!r.u8(&type) || !r.u64(&id) || !r.count(&n, 1)) {
    *error = "bad stats reply: " + r.error();
    return false;
  }
  stats->clear();
  stats->resize(n);
  for (ClassStats &s : *stats)
    if (!launcher::get(r, &s)) break;
  if (!r.finish()) {
    *error = "bad stats reply: " + r.error();
    return false;
  }
  return true;
}

}  // namespace mle
