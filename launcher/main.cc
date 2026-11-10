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
  @file components/mle/launcher/main.cc

  mle_launcher: starts MLE worker processes for the host (mysqld).

  Started by libmle_host with a SOCK_SEQPACKET socket as fd 3 and nothing
  else; receives its configuration as the first message
  (launcher_protocol.h). Single-threaded: a poll loop over the host socket
  and the pidfds of all workers.

  A worker is started with clone3(CLONE_PIDFD | CLONE_INTO_CGROUP), so it
  is in its cgroup leaf from its first instruction and we hold a pidfd. The
  child sets up its descriptors, limits and Landlock and execs the worker
  executable (child.cc). An exec-error pipe makes the spawn synchronous up
  to exec, so failures are reported with their cause.

  The launcher keeps a warm pool per worker class, hands a worker to the
  host as [mailbox memfd, control socket, pidfd], reaps every worker (it is
  the parent) and reports exits of handed-out workers. When the host closes
  the socket, it kills all workers and exits. The launcher does not use
  PR_SET_PDEATHSIG for itself: that signal follows the host *thread* that
  started it, not the host process.
*/

#include <fcntl.h>
#include <linux/sched.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "launcher_internal.h"
#include "mle/protocol.h"

namespace mle::launcher {
namespace {

constexpr int kHostFd = 3;
constexpr size_t kMaxDatagram = 256 * 1024;

void log(const char *fmt, const std::string &arg) {
  fprintf(stderr, "[MLE launcher %d] ", getpid());
  fprintf(stderr, fmt, arg.c_str());
  fputc('\n', stderr);
}

struct Worker {
  uint64_t id = 0;
  size_t cls = 0;
  pid_t pid = -1;
  int pidfd = -1;
  int mailbox = -1;  // held until handed out
  int control = -1;  // host end, held until handed out
  std::string leaf;
  bool handed_out = false;
};

struct Class {
  ClassConfig cfg;
  std::string error;  // non-empty: the class cannot be used
  ClassStats stats;
  std::deque<uint64_t> pool;
};

class Launcher {
 public:
  int run();

 private:
  bool configure(Reader &r);
  void reply_config(bool ok, const std::string &error);
  bool handle_host();
  bool acquire(uint64_t request, uint32_t index);
  bool send_stats(uint64_t request);
  bool send(const std::string &body, const int *fds = nullptr, int nfds = 0);
  Worker *spawn(size_t cls, std::string *error);
  void reap(uint64_t id);
  void shutdown();
  bool need_refill() const;
  void refill_one();

  uint32_t m_arena_size = 0;
  std::string m_cgroup_base;  // empty: no cgroup
  std::string m_cgroup_info;
  int m_landlock_abi = 0;
  std::vector<Class> m_classes;
  std::map<uint64_t, Worker> m_workers;
  uint64_t m_next_id = 0;
  /** Leaves whose processes were still exiting; removed later. */
  std::vector<std::string> m_stale_leaves;
};

// ---------------------------------------------------------------- messages

bool Launcher::send(const std::string &body, const int *fds, int nfds) {
  iovec iov{const_cast<char *>(body.data()), body.size()};
  msghdr m{};
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  alignas(cmsghdr) char cbuf[CMSG_SPACE(4 * sizeof(int))];
  if (nfds > 0) {
    m.msg_control = cbuf;
    m.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
    cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
    memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
  }
  ssize_t n;
  do {
    n = sendmsg(kHostFd, &m, MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  return n == static_cast<ssize_t>(body.size());
}

bool Launcher::configure(Reader &r) {
  uint32_t version, n;
  if (!r.u32(&version) || !r.u32(&m_arena_size) || !r.str(&m_cgroup_base) ||
      !r.count(&n, 1)) {
    reply_config(false, "bad configuration: " + r.error());
    return false;
  }
  if (version != kVersion) {
    reply_config(false, "protocol version " + std::to_string(version) +
                            ", expected " + std::to_string(kVersion));
    return false;
  }
  if (m_arena_size < wire::kMinArenaSize || m_arena_size % 4096 != 0) {
    reply_config(false, "bad arena size");
    return false;
  }
  m_classes.resize(n);
  for (Class &c : m_classes)
    if (!get(r, &c.cfg)) break;
  if (!r.finish()) {
    reply_config(false, "bad configuration: " + r.error());
    return false;
  }

  m_landlock_abi = landlock_abi();
  if (m_cgroup_base.empty()) {
    m_cgroup_info =
        "no cgroup base configured: workers run without memory "
        "and process limits";
  } else if (std::string why = cgroup_prepare(m_cgroup_base); !why.empty()) {
    m_cgroup_info = why + "; workers run without memory and process limits";
    log("%s", m_cgroup_info);
    m_cgroup_base.clear();
  } else {
    m_cgroup_info = m_cgroup_base;
  }

  for (Class &c : m_classes) {
    const ClassConfig &cfg = c.cfg;
    auto absolute_existing = [](const std::string &p) {
      struct stat st;
      return !p.empty() && p[0] == '/' && stat(p.c_str(), &st) == 0;
    };
    if (cfg.name.empty()) {
      c.error = "worker class without a name";
    } else if (cfg.executable.empty() || cfg.executable[0] != '/' ||
               access(cfg.executable.c_str(), X_OK) != 0) {
      c.error = "worker executable '" + cfg.executable +
                "' is not an absolute path to an executable file";
    } else if (m_landlock_abi < kMinLandlockAbi &&
               !cfg.insecure_without_landlock) {
      c.error = "Landlock ABI " + std::to_string(kMinLandlockAbi) +
                " or later (Linux 6.2) is required to isolate workers; "
                "this kernel has " +
                (m_landlock_abi == 0 ? std::string("none")
                                     : std::to_string(m_landlock_abi));
    } else {
      for (const auto *list : {&cfg.read_paths, &cfg.exec_paths})
        for (const std::string &p : *list)
          if (c.error.empty() && !absolute_existing(p))
            c.error =
                "worker path '" + p + "' is not an existing absolute path";
    }
    c.stats.last_error = c.error;
  }
  reply_config(true, "");
  return true;
}

void Launcher::reply_config(bool ok, const std::string &error) {
  std::string body;
  Writer w(&body);
  w.u8(static_cast<uint8_t>(Msg::kConfigReply));
  w.u8(ok ? 1 : 0);
  w.str(error);
  w.u32(static_cast<uint32_t>(m_landlock_abi));
  w.u8(m_cgroup_base.empty() ? 0 : 1);
  w.str(m_cgroup_info);
  w.u32(ok ? static_cast<uint32_t>(m_classes.size()) : 0);
  if (ok)
    for (const Class &c : m_classes) w.str(c.error);
  send(body);
}

bool Launcher::send_stats(uint64_t request) {
  std::string body;
  Writer w(&body);
  w.u8(static_cast<uint8_t>(Msg::kStatsReply));
  w.u64(request);
  w.u32(static_cast<uint32_t>(m_classes.size()));
  for (Class &c : m_classes) {
    c.stats.idle = static_cast<uint32_t>(c.pool.size());
    put(w, c.stats);
  }
  return send(body);
}

/** Returns false when the host is gone or broke the protocol. */
bool Launcher::handle_host() {
  std::string body(kMaxDatagram, '\0');
  ssize_t n;
  do {
    n = recv(kHostFd, body.data(), body.size(), 0);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) return false;
  body.resize(static_cast<size_t>(n));
  Reader r(body);
  uint8_t type;
  uint64_t request;
  if (!r.u8(&type) || !r.u64(&request)) return false;
  switch (static_cast<Msg>(type)) {
    case Msg::kAcquire: {
      uint32_t index;
      if (!r.u32(&index) || !r.finish()) return false;
      return acquire(request, index);
    }
    case Msg::kStats:
      return r.finish() && send_stats(request);
    default:
      log("%s", "unexpected message from the host");
      return false;
  }
}

// ---------------------------------------------------------------- workers

Worker *Launcher::spawn(size_t cls, std::string *error) {
  Class &c = m_classes[cls];
  const uint64_t id = ++m_next_id;
  const size_t region = wire::region_size(m_arena_size);

  Worker w;
  w.id = id;
  w.cls = cls;
  int sv[2] = {-1, -1}, pipefd[2] = {-1, -1}, cgroup_fd = -1;
  auto cleanup = [&](const std::string &what) {
    *error = what;
    for (int fd : {w.mailbox, sv[0], sv[1], pipefd[0], pipefd[1], cgroup_fd})
      if (fd >= 0) close(fd);
    cgroup_remove_leaf(w.leaf);
    return nullptr;
  };

  // The mailbox, with its header, sealed against resizing.
  w.mailbox = memfd_create("mle-mailbox", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (w.mailbox < 0 || ftruncate(w.mailbox, static_cast<off_t>(region)) != 0)
    return cleanup(std::string("mailbox: ") + strerror(errno));
  void *p = mmap(nullptr, wire::kHeaderSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                 w.mailbox, 0);
  if (p == MAP_FAILED) return cleanup(std::string("mmap: ") + strerror(errno));
  auto *h = static_cast<wire::RegionHeader *>(p);
  h->magic = wire::kMagic;
  h->version = wire::kVersion;
  h->region_size = region;
  h->arena_size = m_arena_size;
  munmap(p, wire::kHeaderSize);
  if (fcntl(w.mailbox, F_ADD_SEALS,
            F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
    return cleanup(std::string("seal mailbox: ") + strerror(errno));

  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0 ||
      pipe2(pipefd, O_CLOEXEC) != 0)
    return cleanup(std::string("socketpair: ") + strerror(errno));

  if (!m_cgroup_base.empty()) {
    w.leaf = cgroup_create_leaf(m_cgroup_base, id, c.cfg, error);
    if (w.leaf.empty()) return cleanup(*error);
    cgroup_fd = open(w.leaf.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cgroup_fd < 0)
      return cleanup("open " + w.leaf + ": " + strerror(errno));
  }

  SpawnRequest req{&c.cfg,    m_landlock_abi, w.mailbox, sv[1],
                   pipefd[1], getpid(),       w.leaf};
  clone_args args{};
  args.flags = CLONE_PIDFD | (cgroup_fd >= 0 ? CLONE_INTO_CGROUP : 0);
  args.pidfd = reinterpret_cast<uint64_t>(&w.pidfd);
  args.exit_signal = SIGCHLD;
  args.cgroup = cgroup_fd >= 0 ? static_cast<uint64_t>(cgroup_fd) : 0;
  const long pid = syscall(SYS_clone3, &args, sizeof args);
  if (pid < 0) return cleanup(std::string("clone3: ") + strerror(errno));
  if (pid == 0) child_main(req);

  w.pid = static_cast<pid_t>(pid);
  close(sv[1]);
  sv[1] = -1;
  close(pipefd[1]);
  pipefd[1] = -1;
  if (cgroup_fd >= 0) close(cgroup_fd);
  cgroup_fd = -1;
  w.control = sv[0];
  sv[0] = -1;

  // EOF: exec succeeded (the pipe is close-on-exec). Data: the child failed.
  ChildFailure f{};
  ssize_t n;
  do {
    n = read(pipefd[0], &f, sizeof f);
  } while (n < 0 && errno == EINTR);
  close(pipefd[0]);
  pipefd[0] = -1;
  c.stats.spawned++;
  auto it = m_workers.emplace(id, std::move(w)).first;
  if (n == static_cast<ssize_t>(sizeof f)) {
    *error = std::string("starting worker '") + c.cfg.name +
             "': " + child_step_name(f.step) + ": " + strerror(f.err);
    c.stats.last_error = *error;
    reap(id);  // waits for the child, closes everything
    return nullptr;
  }
  return &it->second;
}

void Launcher::reap(uint64_t id) {
  auto it = m_workers.find(id);
  if (it == m_workers.end()) return;
  Worker &w = it->second;
  siginfo_t si{};
  while (waitid(P_PIDFD, static_cast<id_t>(w.pidfd), &si, WEXITED) < 0 &&
         errno == EINTR) {
  }
  // As a waitpid() status, so the host can use the W* macros.
  int status = 0;
  if (si.si_code == CLD_EXITED)
    status = (si.si_status & 0xff) << 8;
  else
    status = (si.si_status & 0x7f) | (si.si_code == CLD_DUMPED ? 0x80 : 0);
  const uint64_t oom_kills = w.leaf.empty() ? 0 : cgroup_oom_kills(w.leaf);
  if (!w.leaf.empty()) {
    cgroup_kill(w.leaf);  // anything the worker left behind
    if (!cgroup_remove_leaf(w.leaf)) m_stale_leaves.push_back(w.leaf);
  }
  for (int fd : {w.pidfd, w.mailbox, w.control})
    if (fd >= 0) close(fd);

  Class &c = m_classes[w.cls];
  if (w.handed_out) {
    std::string body;
    Writer wr(&body);
    wr.u8(static_cast<uint8_t>(Msg::kExited));
    wr.u64(w.id);
    wr.u32(static_cast<uint32_t>(status));
    wr.u64(oom_kills);
    send(body);  // if the host is gone, the loop notices on its socket
  } else {
    for (auto p = c.pool.begin(); p != c.pool.end(); ++p)
      if (*p == id) {
        c.pool.erase(p);
        c.stats.pool_deaths++;
        c.stats.consecutive_pool_deaths++;
        if (c.stats.last_error.empty() ||
            c.stats.consecutive_pool_deaths >= kMaxPoolDeaths)
          c.stats.last_error = "worker died while idle in the pool";
        break;
      }
  }
  m_workers.erase(it);
}

bool Launcher::acquire(uint64_t request, uint32_t index) {
  std::string body, error;
  Writer w(&body);
  w.u8(static_cast<uint8_t>(Msg::kWorker));
  w.u64(request);

  Worker *worker = nullptr;
  if (index >= m_classes.size()) {
    error = "unknown worker class";
  } else if (!m_classes[index].error.empty()) {
    error = m_classes[index].error;
  } else {
    Class &c = m_classes[index];
    while (worker == nullptr && !c.pool.empty()) {
      const uint64_t id = c.pool.front();
      c.pool.pop_front();
      Worker &candidate = m_workers.at(id);
      pollfd p{candidate.pidfd, POLLIN, 0};
      if (poll(&p, 1, 0) > 0) {  // died while idle
        c.pool.push_front(id);
        reap(id);
        continue;
      }
      worker = &candidate;
    }
    if (worker == nullptr) worker = spawn(index, &error);
  }

  if (worker == nullptr) {
    w.u8(0);
    w.str(error);
    w.u64(0);
    w.u32(0);
    return send(body);
  }
  w.u8(1);
  w.str("");
  w.u64(worker->id);
  w.u32(static_cast<uint32_t>(worker->pid));
  const int fds[3] = {worker->mailbox, worker->control, worker->pidfd};
  if (!send(body, fds, 3)) return false;
  // The host's copies are now the only ones: when it closes the control
  // socket, the worker sees EOF.
  close(worker->mailbox);
  close(worker->control);
  worker->mailbox = worker->control = -1;
  worker->handed_out = true;
  Class &c = m_classes[worker->cls];
  c.stats.handed_out++;
  c.stats.consecutive_pool_deaths = 0;
  return true;
}

bool Launcher::need_refill() const {
  for (const Class &c : m_classes)
    if (c.error.empty() && c.pool.size() < c.cfg.pool_size &&
        c.stats.consecutive_pool_deaths < kMaxPoolDeaths)
      return true;
  return false;
}

void Launcher::refill_one() {
  for (size_t i = 0; i < m_classes.size(); i++) {
    Class &c = m_classes[i];
    if (!c.error.empty() || c.pool.size() >= c.cfg.pool_size ||
        c.stats.consecutive_pool_deaths >= kMaxPoolDeaths)
      continue;
    std::string error;
    if (Worker *w = spawn(i, &error)) {
      c.pool.push_back(w->id);
    } else {
      // Counts as a pool death, so a broken class stops being refilled.
      c.stats.consecutive_pool_deaths++;
      log("%s", error);
    }
    return;  // one per loop turn, so host requests are not delayed
  }
}

void Launcher::shutdown() {
  for (auto &[id, w] : m_workers)
    syscall(SYS_pidfd_send_signal, w.pidfd, SIGKILL, nullptr, 0);
  while (!m_workers.empty()) reap(m_workers.begin()->first);
  for (int i = 0; i < 100 && !m_stale_leaves.empty(); i++) {
    usleep(10000);
    std::erase_if(m_stale_leaves, cgroup_remove_leaf);
  }
}

int Launcher::run() {
  // First message: the configuration.
  {
    std::string body(kMaxDatagram, '\0');
    const ssize_t n = recv(kHostFd, body.data(), body.size(), 0);
    if (n <= 0) return 1;
    body.resize(static_cast<size_t>(n));
    Reader r(body);
    uint8_t type;
    if (!r.u8(&type) || type != static_cast<uint8_t>(Msg::kConfig)) return 1;
    if (!configure(r)) return 1;
  }

  std::vector<pollfd> fds;
  std::vector<uint64_t> ids;
  while (true) {
    const bool refill = need_refill();
    fds.assign(1, pollfd{kHostFd, POLLIN, 0});
    ids.assign(1, 0);
    for (const auto &[id, w] : m_workers) {
      fds.push_back(pollfd{w.pidfd, POLLIN, 0});
      ids.push_back(id);
    }
    std::erase_if(m_stale_leaves, cgroup_remove_leaf);
    const int timeout = refill ? 0 : !m_stale_leaves.empty() ? 50 : -1;
    const int r = poll(fds.data(), fds.size(), timeout);
    if (r < 0 && errno != EINTR) break;
    for (size_t i = 1; i < fds.size(); i++)
      if (fds[i].revents != 0) reap(ids[i]);
    if (fds[0].revents != 0 && !handle_host()) break;
    if (refill) refill_one();
  }
  shutdown();
  return 0;
}

}  // namespace
}  // namespace mle::launcher

int main() {
  // Only fd 3 (and 0-2) are expected; drop anything else defensively.
  syscall(SYS_close_range, 4U, ~0U, 0U);
  signal(SIGPIPE, SIG_IGN);
  prctl(PR_SET_NAME, "mle_launcher");
  mle::launcher::Launcher launcher;
  return launcher.run();
}
