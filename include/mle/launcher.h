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

#ifndef MLE_LAUNCHER_H
#define MLE_LAUNCHER_H

/**
  @file components/mle/include/mle/launcher.h

  Host side of the MLE launcher (libmle_host): starts mle_launcher, asks it
  for worker processes and owns what it hands over (the mapped mailbox, the
  control socket, the pidfd).

  Threads: Launcher is thread-safe. A WorkerProcess is used by one thread
  at a time, except kill() and alive(), which any thread may call.
  Lifetime: a WorkerProcess keeps the launcher channel alive, so it may
  outlive its Launcher object (the launcher process ends when the last of
  them is gone).
*/

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mle/launcher_protocol.h"
#include "mle/protocol.h"

namespace mle {

using launcher::ClassConfig;
using launcher::ClassStats;

struct LauncherConfig {
  /** Path of the mle_launcher executable. */
  std::string launcher_exe;
  /** A cgroup v2 directory delegated to us (e.g. systemd Delegate=yes with
      DelegateSubgroup=), without processes of its own. Empty: no limits. */
  std::string cgroup_base;
  uint32_t arena_size = wire::kDefaultArenaSize;
  std::vector<ClassConfig> classes;
  /** How long start() and acquire() wait for the launcher. */
  int timeout_ms = 10000;
};

struct LauncherInfo {
  pid_t pid = -1;
  int landlock_abi = 0;
  bool cgroup_enabled = false;
  /** Why cgroups are not used, or the base in use. */
  std::string cgroup_info;
  /** Per class (config order): empty if usable, else why not. */
  std::vector<std::string> class_errors;
};

/** How a worker ended, as reported by the launcher (its parent). */
struct ExitInfo {
  bool known = false;
  /** waitpid() status. */
  int status = 0;
  /** oom_kill count of the worker's cgroup. */
  uint64_t oom_kills = 0;
  /** "exited with status 3", "killed by signal 11 (segmentation fault)",
      "killed by the out-of-memory killer (cgroup memory limit)", ... */
  std::string describe() const;
};

class LauncherChannel;

/** A worker process handed over by the launcher. Destroying it kills the
    worker (workers hold no state worth saving and are never reused). */
class WorkerProcess {
 public:
  ~WorkerProcess();
  WorkerProcess(const WorkerProcess &) = delete;
  WorkerProcess &operator=(const WorkerProcess &) = delete;

  uint64_t id() const { return m_id; }
  pid_t pid() const { return m_pid; }
  const std::string &worker_class() const { return m_class; }

  wire::RegionHeader *header() const { return m_header; }
  unsigned char *arena() const {
    return reinterpret_cast<unsigned char *>(m_header) + wire::kHeaderSize;
  }
  uint32_t arena_size() const { return m_arena_size; }
  int control_fd() const { return m_control; }
  int pidfd() const { return m_pidfd; }

  /** False once the process has exited (it may not be reaped yet). */
  bool alive() const;
  /** SIGKILL through the pidfd (no pid reuse race). */
  bool kill() const;
  /** Waits until the process has exited; false on timeout. */
  bool wait_exit(int timeout_ms) const;
  /** Waits for the launcher's exit report; known == false if the worker is
      still running or no report came within timeout_ms. */
  ExitInfo exit_info(int timeout_ms) const;

 private:
  friend class Launcher;
  WorkerProcess() = default;

  std::shared_ptr<LauncherChannel> m_channel;
  std::string m_class;
  uint64_t m_id = 0;
  pid_t m_pid = -1;
  int m_mailbox = -1;
  int m_control = -1;
  int m_pidfd = -1;
  wire::RegionHeader *m_header = nullptr;
  uint32_t m_arena_size = 0;
};

class Launcher {
 public:
  /** Starts mle_launcher and sends it the configuration. nullptr and *error
      on failure. A class that cannot be used (see info().class_errors) does
      not make start() fail; acquire() refuses it. */
  static std::unique_ptr<Launcher> start(const LauncherConfig &config,
                                         std::string *error);
  /** Closes the channel; the launcher kills its pool and exits once no
      WorkerProcess uses the channel any more. */
  ~Launcher();

  const LauncherInfo &info() const;
  /** False once mle_launcher has exited (killed, crashed): acquire() fails
      from then on; start a new Launcher. */
  bool alive() const;

  /** A fresh worker of the class (from the pool, or spawned now). The
      worker may still be starting: wait for its READY in the mailbox. */
  std::unique_ptr<WorkerProcess> acquire(const std::string &worker_class,
                                         std::string *error);

  /** Per class, in config order. */
  bool stats(std::vector<ClassStats> *stats, std::string *error);

 private:
  Launcher() = default;
  std::shared_ptr<LauncherChannel> m_channel;
};

}  // namespace mle

#endif  // MLE_LAUNCHER_H
