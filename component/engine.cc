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

#include "engine.h"

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include "mle/defaults.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_component {

namespace {

/** The one worker class of Phase 4 (defaults.h). */
constexpr char kWorkerClass[] = "jvm";

std::mutex g_mutex;
std::unique_ptr<mle::Launcher> g_launcher;  // guarded by g_mutex
std::string g_error;                        // guarded by g_mutex
std::atomic<long> g_workers_in_use{0};

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string mib(uint64_t bytes) {
  return std::to_string(bytes >> 20) + " MiB";
}

/** The files the engine needs; "" if all are there, else what is missing. */
std::string check_layout(const mle::Layout &l, const Settings &s) {
  struct Need {
    const std::string &path;
    int mode;
    const char *what;
  };
  const std::string libjvm = l.java_home + "/lib/server/libjvm.so";
  const Need needs[] = {
      {l.launcher_exe, X_OK, "mle_launcher"},
      {l.worker_exe, X_OK, "mle_worker"},
      {l.worker_jar, R_OK, "mle-worker.jar"},
      {l.resource_cache, R_OK | X_OK, "resources"},
  };
  for (const Need &n : needs)
    if (access(n.path.c_str(), n.mode) != 0)
      return std::string("the MLE files are not installed in mle.dir '") +
             s.dir + "': " + n.what + " (" + n.path +
             ") is missing or not accessible";
  if (access(libjvm.c_str(), R_OK) != 0)
    return "mle.java_home '" + s.java_home +
           "' is not a GraalVM JDK 25: " + libjvm + " is missing";
  return "";
}

}  // namespace

std::string auto_cgroup_base(std::string *why) {
  const std::string self = read_file("/proc/self/cgroup");
  const size_t at = self.find("0::");
  if (at == std::string::npos) {
    *why = "mysqld is not in a cgroup v2 hierarchy";
    return "";
  }
  const size_t end = self.find('\n', at);
  std::string own = self.substr(at + 3, end == std::string::npos
                                            ? std::string::npos
                                            : end - at - 3);
  const size_t slash = own.rfind('/');
  if (own.empty() || own == "/" || slash == std::string::npos) {
    *why = "mysqld is in the root cgroup";
    return "";
  }
  const std::string parent = "/sys/fs/cgroup" + own.substr(0, slash);
  if (access(parent.c_str(), W_OK) != 0 ||
      access((parent + "/cgroup.procs").c_str(), W_OK) != 0) {
    *why = "the parent of mysqld's cgroup (" + parent +
           ") is not delegated to mysqld (systemd: Delegate=yes, "
           "DelegateSubgroup=)";
    return "";
  }
  if (!read_file(parent + "/cgroup.procs").empty()) {
    *why = "the parent of mysqld's cgroup (" + parent +
           ") has processes of its own";
    return "";
  }
  return parent;
}

bool engine_start(const Settings &s, std::string *error, std::string *info) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_launcher.reset();
  g_error.clear();

  const mle::Layout layout = mle::Layout::installed(s.dir, s.java_home);
  *error = check_layout(layout, s);
  if (!error->empty()) {
    g_error = *error;
    return false;
  }

  std::string cgroup_note;
  mle::LauncherConfig config;
  config.launcher_exe = layout.launcher_exe;
  if (s.cgroup_base == "AUTO" || s.cgroup_base == "auto") {
    config.cgroup_base = auto_cgroup_base(&cgroup_note);
  } else if (s.cgroup_base != "NONE" && s.cgroup_base != "none") {
    config.cgroup_base = s.cgroup_base;
  } else {
    cgroup_note = "mle.cgroup_base is NONE";
  }

  mle::JvmClassOptions options;
  options.name = kWorkerClass;
  options.memory_max = s.memory_max;
  options.heap = std::to_string(s.heap_max >> 20) + "m";
  options.pool_size = s.pool_size;
  config.classes = {mle::jvm_worker_class(layout, options)};

  std::string launcher_error;
  std::unique_ptr<mle::Launcher> launcher =
      mle::Launcher::start(config, &launcher_error);
  if (!launcher) {
    *error = "cannot start mle_launcher: " + launcher_error;
    g_error = *error;
    return false;
  }
  const mle::LauncherInfo &li = launcher->info();
  if (!li.class_errors.empty() && !li.class_errors[0].empty()) {
    *error = "workers cannot run here: " + li.class_errors[0];
    g_error = *error;
    return false;  // destroying the launcher stops it
  }

  std::string cgroup;
  if (li.cgroup_enabled)
    cgroup = "worker cgroups below " + li.cgroup_info + ", memory.max " +
             mib(s.memory_max) + " each";
  else
    cgroup = "no worker cgroups, so mle.memory_max is not enforced (" +
             (cgroup_note.empty() ? li.cgroup_info : cgroup_note) + ")";
  *info = "Engine started: mle_launcher pid " + std::to_string(li.pid) +
          ", Landlock ABI " + std::to_string(li.landlock_abi) + ", " + cgroup +
          ", Java heap " + mib(s.heap_max) + ", pool " +
          std::to_string(s.pool_size) + ", files in " + s.dir +
          ", JDK " + layout.java_home +
          (layout.aot_cache.empty() ? ", no AOT cache" : "") +
          (layout.python_venv.empty() ? ", no Python venv (no numpy)" : "") +
          ".";
  g_launcher = std::move(launcher);
  return true;
}

void engine_stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_launcher.reset();
  g_error.clear();
}

EngineState engine_state(bool with_stats) {
  EngineState st;
  std::lock_guard<std::mutex> lock(g_mutex);
  st.error = g_error;
  if (!g_launcher) return st;
  st.active = true;
  st.launcher_pid = g_launcher->info().pid;
  st.cgroup = g_launcher->info().cgroup_enabled;
  if (with_stats) {
    std::vector<mle::ClassStats> stats;
    std::string error;
    if (g_launcher->stats(&stats, &error) && !stats.empty()) {
      st.pool_idle = stats[0].idle;
      st.spawned = stats[0].spawned;
    }
  }
  return st;
}

long engine_workers_in_use() { return g_workers_in_use.load(); }

namespace {
/** How long a session waits for its worker to be READY (a pool hit is
    ready at once; a cold start takes seconds). */
constexpr int kReadyTimeoutMs = 120000;
}  // namespace

std::unique_ptr<mle::Worker> engine_new_worker(uint32_t max_workers, int spin_us,
                                               int interrupt_grace_ms,
                                               AcquireError *why,
                                               std::string *error) {
  if (++g_workers_in_use > static_cast<long>(max_workers)) {
    --g_workers_in_use;
    *why = AcquireError::kLimit;
    return nullptr;
  }
  std::unique_ptr<mle::WorkerProcess> process;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_launcher) {
      --g_workers_in_use;
      *why = AcquireError::kInactive;
      *error = g_error.empty() ? "the MLE engine is stopped" : g_error;
      return nullptr;
    }
    process = g_launcher->acquire(kWorkerClass, error);
  }
  if (!process) {
    --g_workers_in_use;
    *why = AcquireError::kFailed;
    return nullptr;
  }
  mle::WorkerOptions options;
  options.spin_us = spin_us;
  options.interrupt_grace_ms = interrupt_grace_ms;
  auto worker = std::make_unique<mle::Worker>(std::move(process), options);
  mle::ReadyMsg ready;
  if (worker->wait_ready(kReadyTimeoutMs, &ready, error) != mle::Status::kOk) {
    worker.reset();
    --g_workers_in_use;
    *why = AcquireError::kFailed;
    return nullptr;
  }
  *why = AcquireError::kNone;
  return worker;
}

void engine_release_worker(std::unique_ptr<mle::Worker> worker) {
  if (!worker) return;
  worker.reset();
  --g_workers_in_use;
}

}  // namespace mle_component
