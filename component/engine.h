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
  @file component/engine.h

  The MLE engine of the server: one mle_launcher process with a worker class
  per installed kind (the JVM, CPython and V8 workers: exp design note 63)
  and their warm pools (11-phase4-component.md §3). Started at INSTALL;
  restarted when a setting that shapes workers changes while no session
  holds a worker; stopped at UNINSTALL.

  No MySQL dependencies (libmle_host only). Thread-safe.
*/

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mle {
class Worker;
class CallHooks;
}

namespace mle_component {

/** The worker kinds: a launcher class each (defaults.h; exp design note 63). */
enum class WorkerKind : unsigned { kJvm = 0, kCPython = 1, kV8 = 2 };
constexpr size_t kKinds = 3;

/** The launcher class name of a kind ("jvm", "cpython", "v8"). */
const char *kind_name(WorkerKind kind);

/** What the engine is started with (from the mle.* sysvars). */
struct Settings {
  /** The JVM worker's (mle.memory_max, mle.heap_max, mle.pool_size). */
  uint64_t memory_max = 2ULL << 30;
  uint64_t heap_max = 512ULL << 20;
  /** The CPython worker's (mle.cpython_memory_max, mle.cpython_pool_size). */
  uint64_t cpython_memory_max = 1ULL << 30;
  uint32_t cpython_pool_size = 2;
  /** The V8 worker's (mle.v8_memory_max, mle.v8_heap_max, mle.v8_pool_size). */
  uint64_t v8_memory_max = 1ULL << 30;
  uint64_t v8_heap_max = 512ULL << 20;
  uint32_t v8_pool_size = 2;
  /** glibc's mmap threshold in workers; 0: glibc's adaptive default. */
  uint64_t malloc_mmap_threshold = 4ULL << 20;
  uint32_t pool_size = 2;
  /** Threads of the vector helper (com.percona.mle.api.Vectors) per worker. */
  uint32_t vector_threads = 4;
  /** AUTO, NONE or a cgroup v2 directory. */
  std::string cgroup_base = "AUTO";
  std::string dir;
  std::string java_home;
  /** What pool workers prepare before they are needed (15 §2, exp note 35):
      none, or a list of python, numpy and vectors (parse_prewarm). */
  std::string pool_prewarm = "vectors";
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

/** Why engine_new_worker failed. kIncompatible: the worker speaks another
    major version of the protocol or does not run the language (protocol
    §9.1); it is not at fault, the installation is. */
enum class AcquireError { kNone, kInactive, kLimit, kFailed, kIncompatible };

/**
  A started worker (READY) of a kind that runs language (the worker's
  identifier: "js", "python", "java") for a session, counted in
  engine_workers_in_use() until engine_release_worker(). At most max_workers
  at a time.
*/
std::unique_ptr<mle::Worker> engine_new_worker(WorkerKind kind, const std::string &language,
                                               uint32_t max_workers, int spin_us,
                                               int interrupt_grace_ms,
                                               mle::CallHooks *hooks,
                                               AcquireError *why,
                                               std::string *error);

/**
  A session gives a worker back. With the principal it ran for, a healthy
  idle worker is reset (RESET: no routines, memory trimmed) and kept for a
  later session of the same principal and kind (mle.worker_cache_size,
  mle.worker_cache_timeout; 12 §17); otherwise it is destroyed (killed). A
  worker that recorded at its start an account of its principal that did
  not exist is given back without its principal: never kept (protocol §1).
*/
void engine_release_worker(std::unique_ptr<mle::Worker> worker, WorkerKind kind = WorkerKind::kJvm,
                           const std::string &principal = "",
                           uint64_t mark = 0);

/** An MLE session begins / ends: its mark for engine_release_worker (a
    worker of an account dropped or renamed after the mark is not kept). */
uint64_t engine_session_begin();
void engine_session_end(uint64_t mark);

/**
  A kept worker of this principal and kind that runs language (READY, reset:
  it has no options in force), counted like engine_new_worker()'s; nullptr if
  none (or at max_workers).
*/
std::unique_ptr<mle::Worker> engine_reuse_worker(const std::string &principal, WorkerKind kind,
                                                 const std::string &language,
                                                 uint32_t max_workers);

/** DROP USER / RENAME USER: kept workers of that account are destroyed (a
    new account of the same name must not get them), and the workers that
    sessions hold for it are retired before their next top-level request
    (protocol §1). */
void engine_forget_account(const std::string &principal);

/** CREATE USER, CREATE ROLE or RENAME USER ran: an account may have been
    created under a name. The server does not say which, so the workers
    that recorded at their start an account of their principal that did
    not exist then are retired before their next top-level request
    (protocol §1, Appendix A: "Accounts that change"); no other. */
void engine_account_created();

/** How many accounts were dropped, renamed or created so far; it only
    grows. Lock-free. */
uint64_t engine_forgets();
/** Whether a worker of the principal must be retired: an account of it was
    dropped or renamed after the count of engine_forgets() was since, or,
    when unknown_at_start, an account may have been created since. With an
    account as the principal and the session's mark as since: whether the
    session outlived that account (it is not the one the session knew). */
bool engine_account_forgotten(std::string_view principal, uint64_t since,
                              bool unknown_at_start);

/** Heap in use over all session workers that report one (JVM: the Java
    heap, V8: its heap), % of their summed limits (mle_memory_used). */
long engine_memory_used_percent();

/** mysqld's system time zone as the workers understand it. */
std::string system_time_zone();  // time_zone.hpp

/**
  mle.cgroup_base = AUTO: the parent of mysqld's cgroup, if it is delegated
  to mysqld (writable) and has no processes of its own (systemd Delegate=yes
  with DelegateSubgroup=). Otherwise "" and *why.
*/
std::string auto_cgroup_base(std::string *why);

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_ENGINE_H
