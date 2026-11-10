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

#ifndef COMPONENTS_MLE_COMPONENT_ENGINE_H
#define COMPONENTS_MLE_COMPONENT_ENGINE_H

/**
  @file components/mle/component/engine.h

  The MLE engine of the server: one mle_launcher process with the worker
  class of the current settings and its warm pool (11-phase4-component.md
  §3). Started at INSTALL; restarted when a setting that shapes workers
  changes while no session holds a worker; stopped at UNINSTALL.

  No MySQL dependencies (libmle_host only). Thread-safe.
*/

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>

namespace mle {
class Worker;
}

namespace mle_component {

/** What the engine is started with (from the mle.* sysvars). */
struct Settings {
  uint64_t memory_max = 2ULL << 30;
  uint64_t heap_max = 512ULL << 20;
  uint32_t pool_size = 1;
  /** AUTO, NONE or a cgroup v2 directory. */
  std::string cgroup_base = "AUTO";
  std::string dir;
  std::string java_home;
  /** Third-party packages installed by the DBA (14 §3); "": <dir>/packages. */
  std::string package_dir;
};

struct EngineState {
  bool active = false;
  /** Why the engine is not active (empty if it never started). */
  std::string error;
  pid_t launcher_pid = -1;
  bool cgroup = false;
  /** Workers idle in the pool, and spawned since the engine started. */
  uint32_t pool_idle = 0;
  uint64_t spawned = 0;
  /** Workers kept for reuse, and how often one was reused. */
  uint32_t cached = 0;
  uint64_t cache_hits = 0;
};

/**
  Starts the engine (stopping a running one first). On failure the engine
  is inactive, *error says why. *info describes the started engine for the
  error log.
*/
bool engine_start(const Settings &settings, std::string *error,
                  std::string *info);

/** Stops the engine: the launcher kills its pool and exits. */
void engine_stop();

/** State; with_stats asks the launcher for its pool statistics. */
EngineState engine_state(bool with_stats);

/** Workers held by sessions; settings that shape workers can change only
    while this is 0. */
long engine_workers_in_use();

/**
  mle_packages_refresh(): ends kept workers and restarts the launcher (its
  pool) so that new workers see newly installed packages. False, with
  *error, while sessions hold workers.
*/
bool engine_refresh(std::string *error);

/** Why engine_new_worker failed. */
enum class AcquireError { kNone, kInactive, kLimit, kFailed };

/**
  A started worker (READY) for a session, counted in engine_workers_in_use()
  until engine_release_worker(). At most max_workers at a time.
*/
std::unique_ptr<mle::Worker> engine_new_worker(uint32_t max_workers, int spin_us,
                                               int interrupt_grace_ms,
                                               AcquireError *why,
                                               std::string *error);

/**
  A session gives a worker back. With the principal it ran for, a healthy
  idle worker is reset (RESET: no routines, memory trimmed) and kept for a
  later session of the same principal (mle.worker_cache_size,
  mle.worker_cache_timeout; 12 §17); otherwise it is destroyed (killed).
*/
void engine_release_worker(std::unique_ptr<mle::Worker> worker,
                           const std::string &principal = "");

/**
  A kept worker of this principal (READY, reset), counted like
  engine_new_worker()'s; nullptr if none (or at max_workers).
*/
std::unique_ptr<mle::Worker> engine_reuse_worker(const std::string &principal,
                                                 uint32_t max_workers);

/** Java heap in use over all session workers, % of their summed limits
    (mle_memory_used). */
long engine_memory_used_percent();

/**
  mysqld's system time zone (time_zone = SYSTEM), as Java and the workers
  understand it: an IANA name (TZ=Area/City or /etc/localtime), else the
  current UTC offset "+hh:mm" (POSIX TZ strings like GMT-3 mean UTC+3 to
  libc but UTC-3 to Java).
*/
std::string system_time_zone();

/**
  mle.cgroup_base = AUTO: the parent of mysqld's cgroup, if it is delegated
  to mysqld (writable) and has no processes of its own (systemd Delegate=yes
  with DelegateSubgroup=). Otherwise "" and *why.
*/
std::string auto_cgroup_base(std::string *why);

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_ENGINE_H
