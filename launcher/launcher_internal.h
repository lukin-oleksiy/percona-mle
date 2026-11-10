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

#ifndef MLE_LAUNCHER_INTERNAL_H
#define MLE_LAUNCHER_INTERNAL_H

#include <sys/types.h>

#include <cstdint>
#include <string>

#include "mle/launcher_protocol.h"

namespace mle::launcher {

// ---------------------------------------------------------------- Landlock

/** Landlock ABI of the running kernel; 0 if Landlock is not available. */
int landlock_abi();

/** Child side, between clone and exec: restricts the process to reading
    the system paths, the class's read paths, its own /proc/<pid> and
    cgroup leaf, executing below the class's exec paths and the system
    library paths, and writing /dev/null. Returns 0 or an errno value. */
int apply_landlock(int abi, const ClassConfig &c, const std::string &leaf);

// ---------------------------------------------------------------- cgroup

/** Prepares base for worker leaves: it must be a cgroup v2 directory with
    no processes of its own and the memory and pids controllers available;
    enables them for children and removes stale leaves (mle-w*). Returns an
    empty string or why base cannot be used. */
std::string cgroup_prepare(const std::string &base);

/** Creates the leaf of worker id with the class's limits; returns its path
    or "" (and *error). */
std::string cgroup_create_leaf(const std::string &base, uint64_t id,
                               const ClassConfig &c, std::string *error);

/** oom_kill count from the leaf's memory.events. */
uint64_t cgroup_oom_kills(const std::string &leaf);

/** Removes a leaf; false while it still has processes (retry later). */
bool cgroup_remove_leaf(const std::string &leaf);

/** Kills every process left in the leaf (e.g. children of a worker). */
void cgroup_kill(const std::string &leaf);

// ---------------------------------------------------------------- spawn

/** What the child reports through the exec-error pipe when it fails. */
struct ChildFailure {
  int32_t step;
  int32_t err;
};

enum ChildStep : int32_t {
  kStepPdeathsig = 1,
  kStepFds,
  kStepChdir,
  kStepRlimit,
  kStepLandlock,
  kStepExec,
};

const char *child_step_name(int32_t step);

struct SpawnRequest {
  const ClassConfig *cls;
  int landlock_abi;
  int mailbox;   // memfd, becomes fd 3
  int control;   // worker end of the control socket, becomes fd 4
  int err_pipe;  // write end, O_CLOEXEC
  pid_t launcher_pid;
  std::string leaf;
};

/** Runs in the child after clone3; never returns. */
[[noreturn]] void child_main(const SpawnRequest &r);

}  // namespace mle::launcher

#endif  // MLE_LAUNCHER_INTERNAL_H
