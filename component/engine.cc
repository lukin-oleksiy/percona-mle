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
#include <cerrno>
#include <cstring>

#include <climits>
#include <cstdlib>
#include <ctime>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "mle/defaults.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_component {

void log_info(const std::string &message);  // component.cc
void log_warning(const std::string &message);  // component.cc
/** mysqld's plugin_dir and datadir; false if they cannot be read. */
size_t worker_cache_size();       // settings.cc
uint32_t worker_cache_timeout();  // settings.cc

namespace {

void log_restart(const std::string &message) { log_info(message); }


std::mutex g_mutex;
std::shared_ptr<mle::Launcher> g_launcher;  // guarded by g_mutex
std::string g_error;                        // guarded by g_mutex
Settings g_settings;                        // guarded by g_mutex
/** Workers held by sessions (for mle_memory_used), with the engine
    generation that started them. */
std::map<const mle::Worker *, uint64_t> g_live;  // guarded by g_mutex
/** The accounts dropped, renamed and created (protocol §1), and each MLE
    session's count when it began (its mark). A session that began before
    its account (or a DEFINER's) was forgotten keeps none of that account's
    workers: a new account of that name must not get them. */
AccountLedger g_accounts;  // guarded by g_mutex
/** g_accounts.count() for readers without the mutex. */
std::atomic<uint64_t> g_forgets_now{0};
/** Incremented at every engine (re)start: workers of an older one are never
    kept for reuse (they have the old settings and package view). */
uint64_t g_generation = 0;                  // guarded by g_mutex
/** The last failed restart of a dead launcher (retried at most each second). */
std::chrono::steady_clock::time_point g_last_failed_start;  // guarded by g_mutex
std::atomic<long> g_workers_in_use{0};

/** Reset workers kept for later sessions of the same principal (12 §17);
    newest at the back. */
struct Cached {
  WorkerKind kind;
  std::string principal;
  std::unique_ptr<mle::Worker> worker;
  std::chrono::steady_clock::time_point since;
  uint64_t generation;
};
std::deque<Cached> g_cache;  // guarded by g_mutex
std::atomic<uint64_t> g_cache_hits{0};
/** Workers being RESET to be kept, per principal (exp note 34): a session of
    the same principal that needs a worker meanwhile waits for that one (warm:
    JIT-compiled code, loaded languages) instead of starting a cold one, which
    made back-to-back sessions alternate between two workers. */
std::map<std::string, int> g_releasing;  // guarded by g_mutex
std::condition_variable g_released;
/** At most this long; a RESET normally takes milliseconds. */
constexpr std::chrono::milliseconds kReuseWait{1000};

/** A release in progress for principal, from construction to destruction. */
class Releasing {
 public:
  explicit Releasing(const std::string &principal) : m_principal(principal) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_releasing[m_principal];
  }
  ~Releasing() {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      const auto it = g_releasing.find(m_principal);
      if (it != g_releasing.end() && --it->second <= 0) g_releasing.erase(it);
    }
    g_released.notify_all();
  }
  Releasing(const Releasing &) = delete;
  Releasing &operator=(const Releasing &) = delete;

 private:
  const std::string m_principal;
};
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

/** Whether a kind is installed (its worker is there). */
bool installed(const mle::Layout &l, WorkerKind kind) {
  switch (kind) {
    case WorkerKind::kJvm:
      return access(l.worker_jar.c_str(), R_OK) == 0;
    case WorkerKind::kCPython:
      return !l.cpython_exe.empty();
    case WorkerKind::kV8:
      return !l.v8_exe.empty();
    case WorkerKind::kTest:
#if !defined(NDEBUG) && defined(MLE_TEST_WORKER_EXE)
      return test_worker_enabled() && access(MLE_TEST_WORKER_EXE, X_OK) == 0;
#else
      return false;
#endif
  }
  return false;
}

/** What an installed kind lacks; "" if it can run. mle_worker is only checked
    to exist: the launcher executes it (and reports why it cannot), and under
    SELinux mysqld itself may not (it is mle_worker_exec_t, for the workers'
    domain). */
std::string kind_problem(const mle::Layout &l, const Settings &s, WorkerKind kind) {
  struct Need {
    const std::string &path;
    int mode;
    const char *what;
  };
  if (kind == WorkerKind::kV8 || kind == WorkerKind::kTest) return "";
  if (kind == WorkerKind::kCPython) {
    if (access(l.cpython_code.c_str(), R_OK | X_OK) != 0)
      return "the CPython worker's Python code (" + l.cpython_code + ") is missing or not accessible";
    return "";
  }
  const std::string libjvm = l.java_home + "/lib/server/libjvm.so";
  const Need needs[] = {
      {l.worker_exe, F_OK, "mle_worker"},
      {l.worker_jar, R_OK, "mle-worker.jar"},
      {l.resource_cache, R_OK | X_OK, "resources"},
  };
  for (const Need &n : needs)
    if (access(n.path.c_str(), n.mode) != 0)
      return std::string("the JVM worker's files are not complete in mle.dir '") + s.dir + "': " + n.what +
             " (" + n.path + ") is missing or not accessible";
  if (access(libjvm.c_str(), R_OK) != 0)
    return "mle.java_home '" + s.java_home + "' is not a GraalVM JDK 25: " + libjvm + " is missing";
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
  // Whether it is delegated (writable, no processes of its own) is checked
  // by mle_launcher, which creates the worker cgroups: under SELinux mysqld
  // itself may not write to cgroups (mysqld_t; the launcher runs in
  // mle_launcher_t).
  return "/sys/fs/cgroup" + own.substr(0, slash);
}

/** Starts a launcher with settings s and makes it the engine's; caller holds
    g_mutex. On failure the running engine (if any) stays as it is. */
bool start_locked(const Settings &s, std::string *error, std::string *info) {
  mle::Layout layout = mle::Layout::installed(s.dir, s.java_home);
  if (char real[PATH_MAX]; !s.package_dir.empty() &&
                           realpath(s.package_dir.c_str(), real) != nullptr)
    layout.package_dir = real;  // a missing directory: no packages
  const auto fail = [&](std::string why) {
    *error = std::move(why);
    if (!g_launcher) g_error = *error;
    return false;
  };
  if (access(layout.launcher_exe.c_str(), X_OK) != 0)
    return fail("the MLE files are not installed in mle.dir '" + s.dir + "': mle_launcher (" +
                layout.launcher_exe + ") is missing or not accessible");
  // Every installed worker kind runs its languages (exp design note 63): the
  // JVM worker JAVASCRIPT, JAVA and (with GraalPy, an optional package)
  // PYTHON; the CPython worker CPYTHON; the V8 worker JS. One that is
  // installed but cannot run (a wrong mle.java_home) is an error, as before:
  // the DBA sees it at INSTALL, not as a language gone missing.
  std::vector<WorkerKind> kinds;
  for (const WorkerKind kind : {WorkerKind::kJvm, WorkerKind::kCPython, WorkerKind::kV8, WorkerKind::kTest}) {
    if (!installed(layout, kind)) continue;
    if (std::string why = kind_problem(layout, s, kind); !why.empty()) return fail(why);
    kinds.push_back(kind);
  }
  if (kinds.empty())
    return fail("no MLE worker is installed in mle.dir '" + s.dir + "' (the JVM, CPython or V8 worker)");

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
  options.memory_max = s.memory_max;
  options.heap = std::to_string(s.heap_max >> 20) + "m";
  options.malloc_mmap_threshold = s.malloc_mmap_threshold;
  options.pool_size = s.pool_size;
  options.vector_threads = s.vector_threads;
  // Pool workers start Java (javac) and JavaScript, which are cheap; GraalPy
  // (and numpy) only when mle.pool_prewarm asks (15 §1: Python starts lazily
  // by default, for memory); "vectors" warms the vector helper up after READY
  // while the worker is idle (exp note 35). The CPython worker reads "numpy".
  options.prewarm = s.pool_prewarm == "none" ? "java,js" : "java,js," + s.pool_prewarm;
  if (!server_private_paths(&options.hidden_paths))
    return fail(
        "cannot read mysqld's plugin_dir and datadir (hidden from routines); "
        "install component_mle with INSTALL COMPONENT");
  // JavaScript's local time (Date.toString(), getHours()) follows mysqld's
  // system time zone; the worker cannot read the system's zone itself.
  const std::string zone = system_time_zone();
  options.extra_jvm_options.push_back(
      "-Duser.timezone=" + (zone[0] == '+' || zone[0] == '-' ? "GMT" + zone : zone));
  // Undocumented, for diagnosing worker starts: workers write to mysqld's
  // stderr (the error log) instead of nowhere.
  if (const char *e = getenv("MLE_WORKER_STDERR"); e != nullptr && *e == '1')
    options.inherit_stderr = true;
  // Diagnostics (README): more options for the JVM workers (GC logging and
  // policy, for example).
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
  // Diagnostics: a directory workers may write in (JFR recordings, Truffle
  // logs); the launcher passes it to the sandbox (launcher/child.cc).
  if (const char *d = getenv("MLE_DEBUG_WRITE_DIR"); d != nullptr && *d != '\0') {
    if (*d != '/')
      log_warning(std::string("MLE_DEBUG_WRITE_DIR=") + d + " is ignored: it must be an absolute path");
    else if (access(d, W_OK) != 0)
      log_warning(std::string("MLE_DEBUG_WRITE_DIR=") + d +
                  " is ignored: not a directory mysqld can write (" + strerror(errno) + ")");
    else {
      config.debug_write_dir = d;
      log_warning(std::string("MLE_DEBUG_WRITE_DIR is set: MLE workers may write files in ") + d +
                  " (a diagnostic setting; unset it for production)");
    }
  }
  for (const WorkerKind kind : kinds) {
    mle::JvmClassOptions o = options;
    o.name = kind_name(kind);
    switch (kind) {
      case WorkerKind::kJvm:
        config.classes.push_back(mle::jvm_worker_class(layout, o));
        break;
      case WorkerKind::kCPython:
        o.memory_max = s.cpython_memory_max;
        o.pool_size = s.cpython_pool_size;
        config.classes.push_back(mle::cpython_worker_class(layout, o));
        break;
      case WorkerKind::kV8: {
        o.memory_max = s.v8_memory_max;
        o.pool_size = s.v8_pool_size;
        mle::ClassConfig c = mle::v8_worker_class(layout, o, s.v8_heap_max);
        // Local time as mysqld's (V8 asks ICU, which reads TZ): an IANA name; an offset of whole
        // hours as Etc/GMT-3 (IANA counts west: the sign is the other way). ICU takes no other
        // offsets (+05:30): those keep the host's zone.
        std::string tz = zone;
        if ((zone[0] == '+' || zone[0] == '-') && zone.size() >= 6 && zone.substr(3) == ":00") {
          const int hours = std::stoi(zone.substr(1, 2));
          tz = hours == 0 ? "UTC" : std::string("Etc/GMT") + (zone[0] == '+' ? "-" : "+") + std::to_string(hours);
        }
        if (tz[0] != '+' && tz[0] != '-') c.env.push_back("TZ=" + tz);
        config.classes.push_back(std::move(c));
        break;
      }
      case WorkerKind::kTest: {
#if !defined(NDEBUG) && defined(MLE_TEST_WORKER_EXE)
        // The scripted worker: each routine's body is its script (unittest/proto_script.h).
        mle::ClassConfig c;
        c.name = o.name;
        c.executable = MLE_TEST_WORKER_EXE;
        c.args = {"proto"};
        c.env = {"LANG=C"};
        const std::string exe = MLE_TEST_WORKER_EXE;
        c.exec_paths = {exe.substr(0, exe.rfind('/'))};
        c.memory_max = o.memory_max;
        c.nofile = 256;
        c.inherit_stderr = o.inherit_stderr;
        config.classes.push_back(std::move(c));
#endif
        break;
      }
    }
  }

  std::string launcher_error;
  std::unique_ptr<mle::Launcher> launcher =
      mle::Launcher::start(config, &launcher_error);
  if (!launcher) {
    *error = "cannot start mle_launcher: " + launcher_error;
    if (!g_launcher || !g_launcher->alive()) g_error = *error;
    return false;
  }
  const mle::LauncherInfo &li = launcher->info();
  for (size_t i = 0; i < kinds.size() && i < li.class_errors.size(); i++)
    if (!li.class_errors[i].empty()) {
      *error = "workers cannot run here: " + li.class_errors[i];
      if (!g_launcher || !g_launcher->alive()) g_error = *error;
      return false;  // destroying the launcher stops it
    }
  unsigned languages = 0;
  std::string running;
  for (const WorkerKind kind : kinds) {
    unsigned bits = kind_languages(kind);
    if (kind == WorkerKind::kJvm && !layout.python) bits &= ~unsigned{kPython};
    languages |= bits;
    std::string what;
    switch (kind) {
      case WorkerKind::kJvm:
        what = "JVM worker (JDK " + layout.java_home + (layout.aot_cache.empty() ? ", no AOT cache" : "") +
               ", Java heap " + mib(s.heap_max) +
               (!layout.python                ? ", no Python (package percona-mle-python)"
                : layout.python_venv.empty() ? ", no Python venv (no numpy)"
                                              : "") +
               ")";
        break;
      case WorkerKind::kCPython:
        what = "CPython worker (" + layout.cpython_exe +
               (layout.cpython_site.empty() ? ", no site-packages: no numpy" : "") + ")";
        break;
      case WorkerKind::kV8:
        what = "V8 worker (" + layout.v8_exe + ", V8 heap " + mib(s.v8_heap_max) + ")";
        break;
      case WorkerKind::kTest:
        what = "the scripted test worker (mle.debug_test_worker)";
        break;
    }
    running += (running.empty() ? "" : ", ") + what;
  }

  std::string cgroup;
  if (li.cgroup_enabled)
    cgroup = "worker cgroups below " + li.cgroup_info + ", memory.max " + mib(s.memory_max) +
             " (JVM), " + mib(s.cpython_memory_max) + " (CPython), " + mib(s.v8_memory_max) + " (V8)";
  else {
    cgroup = "no worker cgroups, so the mle.*memory_max limits are not enforced (" +
             (cgroup_note.empty() ? li.cgroup_info : cgroup_note) + ")";
    // Workers then have no memory or thread limit of their own (only their
    // heaps; their OOM score makes the kernel pick them before mysqld).
    log_warning("MLE workers run without cgroups: " + cgroup +
                ". Delegate a cgroup to mysqld (systemd: Delegate=yes) or set "
                "mle.cgroup_base.");
  }
  *info = "Engine started: mle_launcher pid " + std::to_string(li.pid) + ", Landlock ABI " +
          std::to_string(li.landlock_abi) + ", " + cgroup + ", files in " + s.dir + "; " + running +
          (layout.package_dir.empty() ? "" : "; packages in " + layout.package_dir) + ".";
  // The new engine: kept workers have the old settings; running sessions'
  // workers keep the old launcher alive until they are released (and are
  // not kept then: an older generation).
  { auto old = take_cache_locked(); }
  g_launcher = std::move(launcher);
  set_installed_languages(languages);
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
    // A launcher that died is restarted by the next call that needs a
    // worker; until that works MLE is not active (g_error says why).
    st.active = g_launcher->alive();
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
    if (launcher->stats(&stats, &error))
      for (const mle::ClassStats &c : stats) {
        st.pool_idle += c.idle;
        st.spawned += c.spawned;
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

std::unique_ptr<mle::Worker> engine_new_worker(WorkerKind kind, const std::string &language,
                                               uint32_t max_workers, int spin_us,
                                               int interrupt_grace_ms,
                                               mle::CallHooks *hooks,
                                               AcquireError *why,
                                               std::string *error) {
  if (++g_workers_in_use > static_cast<long>(max_workers)) {
    --g_workers_in_use;
    *why = AcquireError::kLimit;
    return nullptr;
  }
  // Given back on every way out but the last, an exception (no memory) too.
  struct InUse {
    bool keep = false;
    ~InUse() {
      if (!keep) --g_workers_in_use;
    }
  } in_use;
  std::unique_ptr<mle::WorkerProcess> process;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_launcher) {
      *why = AcquireError::kInactive;
      *error = g_error.empty() ? "the MLE engine is stopped" : g_error;
      return nullptr;
    }
    process = g_launcher->acquire(kind_name(kind), error);
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
        process = g_launcher->acquire(kind_name(kind), error);
      } else {
        g_last_failed_start = now;
      }
    }
    generation = g_generation;
  }
  if (!process) {
    *why = AcquireError::kFailed;
    return nullptr;
  }
  mle::WorkerOptions options;
  options.spin_us = spin_us;
  options.interrupt_grace_ms = interrupt_grace_ms;
  auto worker = std::make_unique<mle::Worker>(std::move(process), options);
  mle::ReadyMsg ready;
  if (const mle::Outcome outcome =
          worker->wait_ready(kReadyTimeoutMs, &ready, error, hooks, language);
      outcome != mle::Outcome::kOk) {
    worker.reset();
    *why = outcome == mle::Outcome::kIncompatible ? AcquireError::kIncompatible
                                                  : AcquireError::kFailed;
    return nullptr;
  }
  *why = AcquireError::kNone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_live[worker.get()] = generation;
  }
  in_use.keep = true;
  return worker;
}

uint64_t engine_session_begin() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_accounts.begin_session();
}

void engine_session_end(uint64_t mark) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_accounts.end_session(mark);
}

void engine_release_worker(std::unique_ptr<mle::Worker> worker, WorkerKind kind,
                           const std::string &principal, uint64_t mark) {
  if (!worker) return;
  // The session's forced GCs, before RESET (which restarts the worker's count).
  g_counters.memory_gcs += static_cast<long>(worker->stats().memory_gcs);
  uint64_t generation = 0;
  bool current = false;
  // Under g_mutex: whether an account of the principal was dropped,
  // renamed or created since the session began.
  const auto forgotten = [&principal, mark] {
    return g_accounts.retire(principal, mark, false);
  };
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_live.find(worker.get());
    if (it != g_live.end()) {
      generation = it->second;
      current = generation == g_generation && !forgotten();
      g_live.erase(it);
    }
  }
  // Only a healthy worker of the current engine that is not running
  // anything is kept, after RESET (it forgets the session's routines and
  // trims its heap).
  // A worker released without its principal recorded an account of it
  // that did not exist when it started: never kept (protocol §1).
  const bool keep = current && !principal.empty() && worker_cache_size() > 0 &&
                    worker->usable() && worker->depth() == 0;
  // Until the worker is kept (or not), a new session of this principal waits
  // for it (engine_reuse_worker). Registered before the worker stops counting
  // as held: once mle_workers no longer counts it, a new session finds it
  // kept or being reset, never neither.
  std::optional<Releasing> releasing;
  if (keep) releasing.emplace(principal);
  --g_workers_in_use;
  if (keep) {
    std::vector<std::unique_ptr<mle::Worker>> evicted;
    {
      std::string error;
      // A worker that must not be reused (a routine left a thread running)
      // says so with the RESULT of RESET: it is retired then, not usable.
      if (worker->reset(&error) == mle::Outcome::kOk && worker->usable()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_launcher && generation == g_generation && !forgotten()) {
          g_cache.push_back({kind, principal, std::move(worker),
                             std::chrono::steady_clock::now(), generation});
          while (g_cache.size() > worker_cache_size()) {
            evicted.push_back(std::move(g_cache.front().worker));
            g_cache.pop_front();
          }
        }
      }
    }
    releasing.reset();  // kept (or not): waiting sessions look again
    evicted.clear();    // ends them, without the mutex
    if (!worker) return;
  }
  worker.reset();
}

std::unique_ptr<mle::Worker> engine_reuse_worker(const std::string &principal, WorkerKind kind,
                                                 const std::string &language,
                                                 uint32_t max_workers) {
  std::unique_ptr<mle::Worker> worker;
  uint64_t generation = 0;
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    const auto deadline = std::chrono::steady_clock::now() + kReuseWait;
    while (true) {
      for (auto it = g_cache.end(); it != g_cache.begin();) {
        --it;
        if (it->principal != principal || it->kind != kind) continue;
        // A worker of the kind that lacks the language (a JVM worker without
        // Python) stays kept for a request it can serve.
        const auto &languages = it->worker->languages();
        if (std::find(languages.begin(), languages.end(), language) == languages.end())
          continue;
        worker = std::move(it->worker);
        generation = it->generation;
        g_cache.erase(it);
        // Held from here, under the mutex: a refresh or SET GLOBAL that
        // checks for held workers sees it.
        ++g_workers_in_use;
        break;
      }
      // None kept: wait while one of this principal is being reset.
      if (worker || g_releasing.count(principal) == 0 ||
          g_released.wait_until(lock, deadline) == std::cv_status::timeout)
        break;
    }
  }
  if (!worker) return nullptr;
  // usable() is what the worker last did; alive() is the process now.
  const bool healthy = worker->usable() && worker->process().alive();
  if (!healthy || g_workers_in_use > static_cast<long>(max_workers)) {
    --g_workers_in_use;
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
    g_forgets_now.store(g_accounts.forget(principal));
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      if (principal_has_account(it->principal, principal)) {
        gone.push_back(std::move(it->worker));
        it = g_cache.erase(it);
      } else {
        ++it;
      }
    }
  }
  gone.clear();  // kills them, without the mutex
}

void engine_account_created() {
  // Kept workers recorded no account that did not exist (such a worker is
  // not kept): none is retired. Sessions' workers are, if they did, before
  // their next top-level request (engine_account_forgotten).
  std::lock_guard<std::mutex> lock(g_mutex);
  g_forgets_now.store(g_accounts.created());
}

uint64_t engine_forgets() { return g_forgets_now.load(); }

bool engine_account_forgotten(std::string_view principal, uint64_t since,
                              bool unknown_at_start) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_accounts.retire(principal, since, unknown_at_start);
}

long engine_memory_used_percent() {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint64_t used = 0, limits = 0;
  // The figures come from memory the workers write: none counts for more
  // than its limit. Workers without a heap of their own (CPython) report none.
  for (const auto &[w, generation] : g_live) {
    const auto st = w->stats();
    const uint64_t limit = st.heap_max != 0 ? st.heap_max : st.heap_used != 0 ? g_settings.heap_max : 0;
    if (limit == 0) continue;
    used += std::min<uint64_t>(st.heap_used, limit);
    limits += limit;
  }
  return limits == 0 ? 0 : static_cast<long>(used * 100 / limits);
}

const char *kind_name(WorkerKind kind) {
  switch (kind) {
    case WorkerKind::kJvm:
      return "jvm";
    case WorkerKind::kCPython:
      return "cpython";
    case WorkerKind::kV8:
      return "v8";
    case WorkerKind::kTest:
      return "test";
  }
  return "";
}

}  // namespace mle_component
