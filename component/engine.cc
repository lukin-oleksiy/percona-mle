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
#include "mle_component.h"

#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <ctime>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "mle/defaults.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_component {

void log_info(const std::string &message);  // component.cc
void log_warning(const std::string &message);  // component.cc
std::vector<std::string> server_private_paths();  // session.cc
size_t worker_cache_size();       // settings.cc
uint32_t worker_cache_timeout();  // settings.cc

namespace {

void log_restart(const std::string &message) { log_info(message); }

/** The one worker class of Phase 4 (defaults.h). */
constexpr char kWorkerClass[] = "jvm";

std::mutex g_mutex;
std::shared_ptr<mle::Launcher> g_launcher;  // guarded by g_mutex
std::string g_error;                        // guarded by g_mutex
Settings g_settings;                        // guarded by g_mutex
/** Workers held by sessions (for mle_memory_used), with the engine
    generation that started them. */
std::map<const mle::Worker *, uint64_t> g_live;  // guarded by g_mutex
/** Incremented at every engine (re)start: workers of an older one are never
    kept for reuse (they have the old settings and package view). */
uint64_t g_generation = 0;                  // guarded by g_mutex
/** The last failed restart of a dead launcher (retried at most each second). */
std::chrono::steady_clock::time_point g_last_failed_start;  // guarded by g_mutex
std::atomic<long> g_workers_in_use{0};

/** Reset workers kept for later sessions of the same principal (12 §17);
    newest at the back. */
struct Cached {
  std::string principal;
  std::unique_ptr<mle::Worker> worker;
  std::chrono::steady_clock::time_point since;
  uint64_t generation;
};
std::deque<Cached> g_cache;  // guarded by g_mutex
std::atomic<uint64_t> g_cache_hits{0};
/** Ends kept workers idle longer than mle.worker_cache_timeout. */
std::thread g_reaper;
std::condition_variable g_reaper_cv;
bool g_reaper_stop = false;  // guarded by g_mutex

/** Takes every kept worker out (to be destroyed without the mutex). */
std::vector<std::unique_ptr<mle::Worker>> take_cache_locked() {
  std::vector<std::unique_ptr<mle::Worker>> out;
  for (Cached &c : g_cache) out.push_back(std::move(c.worker));
  g_cache.clear();
  return out;
}

void reaper() {
  std::unique_lock<std::mutex> lock(g_mutex);
  while (!g_reaper_stop) {
    g_reaper_cv.wait_for(lock, std::chrono::seconds(1));
    const auto limit = std::chrono::seconds(worker_cache_timeout());
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<mle::Worker>> expired;
    while (!g_cache.empty() && (now - g_cache.front().since >= limit ||
                                g_cache.size() > worker_cache_size())) {
      expired.push_back(std::move(g_cache.front().worker));
      g_cache.pop_front();
    }
    if (expired.empty()) continue;
    lock.unlock();
    expired.clear();  // kills the processes
    lock.lock();
  }
}

void start_reaper_locked() {
  if (g_reaper.joinable()) return;
  g_reaper_stop = false;
  g_reaper = std::thread(reaper);
}

/** Stops the reaper; caller does not hold g_mutex. */
void stop_reaper() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_reaper_stop = true;
  }
  g_reaper_cv.notify_all();
  if (g_reaper.joinable()) g_reaper.join();
}

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

std::string system_time_zone() {
  if (const char *tz = getenv("TZ"); tz != nullptr && *tz != '\0') {
    const std::string zone = tz[0] == ':' ? tz + 1 : tz;
    if (zone.find('/') != std::string::npos && zone[0] != '/') return zone;
    if (zone == "UTC") return zone;
  } else {
    char target[PATH_MAX];
    const ssize_t n = readlink("/etc/localtime", target, sizeof target - 1);
    if (n > 0) {
      const std::string path(target, static_cast<size_t>(n));
      const size_t at = path.find("zoneinfo/");
      if (at != std::string::npos) return path.substr(at + 9);
    }
  }
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  const long off = local.tm_gmtoff;
  char b[48];
  snprintf(b, sizeof b, "%c%02ld:%02ld", off < 0 ? '-' : '+', labs(off) / 3600,
           labs(off) % 3600 / 60);
  return b;
}

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

/** Starts a launcher with settings s and makes it the engine's; caller holds
    g_mutex. On failure the running engine (if any) stays as it is. */
bool start_locked(const Settings &s, std::string *error, std::string *info) {
  mle::Layout layout = mle::Layout::installed(s.dir, s.java_home);
  if (char real[PATH_MAX]; !s.package_dir.empty() &&
                           realpath(s.package_dir.c_str(), real) != nullptr)
    layout.package_dir = real;  // a missing directory: no packages
  // Python is an optional package: without GraalPy, PYTHON routines are an
  // unsupported language.
  set_installed_languages(layout.python ? kAllLanguages
                                        : kAllLanguages & ~kPython);
  *error = check_layout(layout, s);
  if (!error->empty()) {
    if (!g_launcher) g_error = *error;
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
  if (s.pool_prewarm != "none") options.prewarm = s.pool_prewarm;
  options.hidden_paths = server_private_paths();
  // JavaScript's local time (Date.toString(), getHours()) follows mysqld's
  // system time zone; the worker cannot read the system's zone itself.
  const std::string zone = system_time_zone();
  options.extra_jvm_options.push_back(
      "-Duser.timezone=" + (zone[0] == '+' || zone[0] == '-' ? "GMT" + zone : zone));
  // Undocumented, for measurements: more JVM options for the workers.
  if (const char *extra = getenv("MLE_JVM_OPTIONS"); extra != nullptr) {
    std::string word;
    for (const char *c = extra;; c++) {
      if (*c == ' ' || *c == '\0') {
        if (!word.empty()) options.extra_jvm_options.push_back(word);
        word.clear();
        if (*c == '\0') break;
      } else {
        word += *c;
      }
    }
  }
  config.classes = {mle::jvm_worker_class(layout, options)};

  std::string launcher_error;
  std::unique_ptr<mle::Launcher> launcher =
      mle::Launcher::start(config, &launcher_error);
  if (!launcher) {
    *error = "cannot start mle_launcher: " + launcher_error;
    if (!g_launcher || !g_launcher->alive()) g_error = *error;
    return false;
  }
  const mle::LauncherInfo &li = launcher->info();
  if (!li.class_errors.empty() && !li.class_errors[0].empty()) {
    *error = "workers cannot run here: " + li.class_errors[0];
    if (!g_launcher || !g_launcher->alive()) g_error = *error;
    return false;  // destroying the launcher stops it
  }

  std::string cgroup;
  if (li.cgroup_enabled)
    cgroup = "worker cgroups below " + li.cgroup_info + ", memory.max " +
             mib(s.memory_max) + " each";
  else {
    cgroup = "no worker cgroups, so mle.memory_max is not enforced (" +
             (cgroup_note.empty() ? li.cgroup_info : cgroup_note) + ")";
    // Workers then have no memory or thread limit of their own (only the
    // Java heap; their OOM score makes the kernel pick them before mysqld).
    log_warning("MLE workers run without cgroups: " + cgroup +
                ". Delegate a cgroup to mysqld (systemd: Delegate=yes) or set "
                "mle.cgroup_base.");
  }
  *info = "Engine started: mle_launcher pid " + std::to_string(li.pid) +
          ", Landlock ABI " + std::to_string(li.landlock_abi) + ", " + cgroup +
          ", Java heap " + mib(s.heap_max) + ", pool " +
          std::to_string(s.pool_size) + ", files in " + s.dir +
          ", JDK " + layout.java_home +
          (layout.aot_cache.empty() ? ", no AOT cache" : "") +
          (!layout.python ? ", no Python (package percona-server-mle-python)"
           : layout.python_venv.empty() ? ", no Python venv (no numpy)" : "") +
          (layout.package_dir.empty() ? "" : ", packages in " + layout.package_dir) +
          ".";
  // The new engine: kept workers have the old settings; running sessions'
  // workers keep the old launcher alive until they are released (and are
  // not kept then: an older generation).
  { auto old = take_cache_locked(); }
  g_launcher = std::move(launcher);
  g_settings = s;
  g_error.clear();
  ++g_generation;
  return true;
}

bool engine_start(const Settings &s, std::string *error, std::string *info) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool ok = start_locked(s, error, info);
  if (ok) start_reaper_locked();
  return ok;
}

void engine_stop() {
  stop_reaper();
  std::vector<std::unique_ptr<mle::Worker>> kept;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    kept = take_cache_locked();
  }
  kept.clear();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_launcher.reset();
  g_error.clear();
}

EngineState engine_state(bool with_stats) {
  EngineState st;
  std::shared_ptr<mle::Launcher> launcher;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    st.error = g_error;
    if (!g_launcher) return st;
    launcher = g_launcher;
    st.active = true;
    st.launcher_pid = g_launcher->info().pid;
    st.cgroup = g_launcher->info().cgroup_enabled;
    st.cached = static_cast<uint32_t>(g_cache.size());
    st.cache_hits = g_cache_hits.load();
  }
  // The round trip to mle_launcher without the engine's mutex (SHOW STATUS
  // must not hold up sessions taking workers).
  if (with_stats) {
    std::vector<mle::ClassStats> stats;
    std::string error;
    if (launcher->stats(&stats, &error) && !stats.empty()) {
      st.pool_idle = stats[0].idle;
      st.spawned = stats[0].spawned;
    }
  }
  return st;
}

long engine_workers_in_use() { return g_workers_in_use.load(); }

bool engine_refresh(std::string *error) {
  std::vector<std::unique_ptr<mle::Worker>> kept;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_launcher) {
    *error = "the MLE engine is not running";
    return false;
  }
  // A refresh is for new packages: refused while sessions use workers, so
  // that they do not mix package versions (their workers would keep the old
  // launcher alive and the old packages).
  if (g_workers_in_use.load() > 0) {
    *error = std::to_string(g_workers_in_use.load()) +
             " session workers are in use; try again when the sessions ended "
             "(or after mle_session_reset() in them)";
    return false;
  }
  kept = take_cache_locked();
  std::string info;
  const Settings settings = g_settings;
  return start_locked(settings, error, &info);
}

namespace {
/** How long a session waits for its worker to be READY (a pool hit is
    ready at once; a cold start takes seconds). */
constexpr int kReadyTimeoutMs = 120000;
}  // namespace

std::unique_ptr<mle::Worker> engine_new_worker(uint32_t max_workers, int spin_us,
                                               int interrupt_grace_ms,
                                               mle::CallHooks *hooks,
                                               AcquireError *why,
                                               std::string *error) {
  if (++g_workers_in_use > static_cast<long>(max_workers)) {
    --g_workers_in_use;
    *why = AcquireError::kLimit;
    return nullptr;
  }
  std::unique_ptr<mle::WorkerProcess> process;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_launcher) {
      --g_workers_in_use;
      *why = AcquireError::kInactive;
      *error = g_error.empty() ? "the MLE engine is stopped" : g_error;
      return nullptr;
    }
    process = g_launcher->acquire(kWorkerClass, error);
    const auto now = std::chrono::steady_clock::now();
    if (!process && !g_launcher->alive() &&
        now - g_last_failed_start >= std::chrono::seconds(1)) {
      // mle_launcher ended (killed, crashed); its workers died with it.
      // Start a new one with the same settings and try once more (a failed
      // start is retried by a later session, at most once a second).
      log_restart("mle_launcher ended unexpectedly; starting it again.");
      std::string info;
      const Settings settings = g_settings;
      if (start_locked(settings, error, &info)) {
        log_restart(info);
        process = g_launcher->acquire(kWorkerClass, error);
      } else {
        g_last_failed_start = now;
      }
    }
    generation = g_generation;
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
  if (worker->wait_ready(kReadyTimeoutMs, &ready, error, hooks) !=
      mle::Status::kOk) {
    worker.reset();
    --g_workers_in_use;
    *why = AcquireError::kFailed;
    return nullptr;
  }
  *why = AcquireError::kNone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_live[worker.get()] = generation;
  }
  return worker;
}

void engine_release_worker(std::unique_ptr<mle::Worker> worker,
                           const std::string &principal) {
  if (!worker) return;
  uint64_t generation = 0;
  bool current = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_live.find(worker.get());
    if (it != g_live.end()) {
      generation = it->second;
      current = generation == g_generation;
      g_live.erase(it);
    }
  }
  --g_workers_in_use;
  // Only a healthy worker of the current engine that is not running
  // anything is kept, after RESET (it forgets the session's routines and
  // trims its heap).
  if (current && !principal.empty() && worker_cache_size() > 0 && worker->usable() &&
      !worker->stats().tainted && worker->depth() == 0) {
    std::string error;
    // RESET also tells whether a routine left a thread running (tainted).
    if (worker->reset(&error) == mle::Status::kOk && worker->usable() &&
        !worker->stats().tainted) {
      std::vector<std::unique_ptr<mle::Worker>> evicted;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_launcher && generation == g_generation) {
          g_cache.push_back({principal, std::move(worker),
                             std::chrono::steady_clock::now(), generation});
          while (g_cache.size() > worker_cache_size()) {
            evicted.push_back(std::move(g_cache.front().worker));
            g_cache.pop_front();
          }
        }
      }
      evicted.clear();
      if (!worker) return;
    }
  }
  worker.reset();
}

std::unique_ptr<mle::Worker> engine_reuse_worker(const std::string &principal,
                                                 uint32_t max_workers) {
  std::unique_ptr<mle::Worker> worker;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_cache.end(); it != g_cache.begin();) {
      --it;
      if (it->principal != principal) continue;
      worker = std::move(it->worker);
      generation = it->generation;
      g_cache.erase(it);
      break;
    }
  }
  if (!worker) return nullptr;
  // usable() is what the worker last did; alive() is the process now.
  const bool healthy = worker->usable() && worker->process().alive();
  if (!healthy || ++g_workers_in_use > static_cast<long>(max_workers)) {
    if (healthy) --g_workers_in_use;
    return nullptr;  // destroyed: died while kept, or no room
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_live[worker.get()] = generation;
  }
  ++g_cache_hits;
  return worker;
}

void engine_forget_account(const std::string &principal) {
  std::vector<std::unique_ptr<mle::Worker>> gone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      if (it->principal == principal) {
        gone.push_back(std::move(it->worker));
        it = g_cache.erase(it);
      } else {
        ++it;
      }
    }
  }
  gone.clear();  // kills them, without the mutex
}

long engine_memory_used_percent() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_live.empty() || g_settings.heap_max == 0) return 0;
  uint64_t used = 0;
  for (const auto &[w, generation] : g_live) used += w->stats().heap_used;
  return static_cast<long>(used * 100 / (g_settings.heap_max * g_live.size()));
}

}  // namespace mle_component
