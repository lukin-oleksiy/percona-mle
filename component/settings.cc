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
  @file component/settings.cc

  The mle.* system variables (11-phase4-component.md §6).

  - Settings that shape workers (memory_max, heap_max, pool_size) can be
    changed while no session holds a worker, else 6116 (as Oracle's
    mle.memory_max "while the component is active"). A change restarts the
    engine, so INSTALL COMPONENT ... SET GLOBAL mle.memory_max = ... works
    (the server applies those values after the component started).
  - dir, java_home and cgroup_base say what mysqld executes and where:
    read-only, like plugin_dir (command line, my.cnf, SET PERSIST_ONLY).
  - languages, max_workers, spin_us, interrupt_grace_ms: dynamic.
*/

#include <cctype>
#include <strings.h>

#include <atomic>
#include <cstring>
#include <unistd.h>
#include <string>

#include <my_inttypes.h>
#include <mysql/components/component_implementation.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_runtime_warning.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysqld_error.h>

#include "mle_component.h"

extern REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
extern REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_warning);

namespace mle_component {

namespace {

constexpr const char kAllLanguageNames[] = "JAVASCRIPT,PYTHON,JAVA";

/* Variable values (written by the server). */
ulonglong g_memory_max = 0;
ulonglong g_heap_max = 0;
uint g_pool_size = 0;
uint g_max_workers = 0;
uint g_worker_cache_size = 0;
uint g_worker_cache_timeout = 0;
uint g_spin_us = 0;
uint g_interrupt_grace_ms = 0;
ulonglong g_sql_result_max = 0;
ulonglong g_sql_row_max = 0;
uint g_sql_statements_max = 0;
uint g_sql_chunk_rows = 0;
char *g_languages = nullptr;
char *g_cgroup_base = nullptr;
char *g_dir = nullptr;
char *g_java_home = nullptr;
char *g_package_dir = nullptr;
char *g_pool_prewarm = nullptr;
char *g_worker = nullptr;

std::atomic<unsigned> g_language_bits{kAllLanguages};
/** What is installed (Python is an optional package): set at engine start. */
std::atomic<unsigned> g_installed_bits{kAllLanguages};

/** Default of mle.dir; must outlive the registration. */
std::string g_default_dir;

std::string text(const char *s) { return s == nullptr ? "" : s; }

/** Engine restart after a setting changed; a failure leaves the engine
    inactive (mle_status) and is reported as a warning and in the log. */
void restart_engine(MYSQL_THD thd, const char *variable) {
  std::string error, info;
  if (engine_start(current_settings(), &error, &info)) {
    log_info(std::string("mle.") + variable + " changed. " + info);
    return;
  }
  const std::string message = std::string("MLE: mle.") + variable +
                              " changed, but the engine cannot start: " +
                              error;
  log_error(message);
  mysql_service_mysql_runtime_warning->emit(thd, ER_LANGUAGE_COMPONENT,
                                            message.c_str());
}

/** Check of an integer setting that shapes workers: refused while a
    session holds a worker; range checked here (a custom check replaces the
    server's range check; the server rejects non-integers before). */
template <typename T, const char *Name, ulonglong Min, ulonglong Max>
int check_shaping(MYSQL_THD, SYS_VAR *, void *save,
                  struct st_mysql_value *value) {
  if (engine_workers_in_use() > 0) {
    my_error(ER_LANGUAGE_COMPONENT_SET_SYSTEM_VARIABLE, MYF(0),
             (std::string("mle.") + Name).c_str());
    return 1;
  }
  long long v = 0;
  if (value->val_int(value, &v) != 0) return 1;
  if (v < 0 && !value->is_unsigned(value)) return 1;
  const auto u = static_cast<ulonglong>(v);
  if (u < Min || u > Max) return 1;  // ER_WRONG_VALUE_FOR_VAR
  *static_cast<T *>(save) = static_cast<T>(u);
  return 0;
}

template <typename T, const char *Name>
void update_shaping(MYSQL_THD thd, SYS_VAR *, void *val_ptr,
                    const void *save) {
  *static_cast<T *>(val_ptr) = *static_cast<const T *>(save);
  restart_engine(thd, Name);
}

constexpr const char kMemoryMax[] = "memory_max";
constexpr const char kHeapMax[] = "heap_max";
constexpr const char kPoolSize[] = "pool_size";

constexpr ulonglong kMiB = 1ULL << 20;
constexpr ulonglong kMemoryMaxMin = 256 * kMiB;
constexpr ulonglong kMemoryMaxMax = 1ULL << 40;
constexpr ulonglong kHeapMaxMin = 64 * kMiB;
constexpr ulonglong kHeapMaxMax = 64ULL << 30;
constexpr ulonglong kPoolSizeMax = 64;

int check_languages(MYSQL_THD, SYS_VAR *, void *save,
                    struct st_mysql_value *value) {
  int length = 0;
  /* Without a buffer the value is copied to the THD's memory and lives
     until the end of the statement. */
  const char *s = value->val_str(value, nullptr, &length);
  unsigned bits = 0;
  if (s == nullptr ||
      !parse_languages({s, static_cast<size_t>(length)}, &bits))
    return 1;
  *static_cast<const char **>(save) = s;
  return 0;
}

/** mle.pool_prewarm: none, python or numpy; shapes workers (refused while a
    session holds one). */
int check_prewarm(MYSQL_THD, SYS_VAR *, void *save, struct st_mysql_value *value) {
  if (engine_workers_in_use() > 0) {
    my_error(ER_LANGUAGE_COMPONENT_SET_SYSTEM_VARIABLE, MYF(0), "mle.pool_prewarm");
    return 1;
  }
  int length = 0;
  const char *s = value->val_str(value, nullptr, &length);
  if (s == nullptr) return 1;
  std::string v(s, static_cast<size_t>(length));
  for (char &c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (v != "none" && v != "python" && v != "numpy") return 1;
  *static_cast<const char **>(save) = s;
  return 0;
}

void update_prewarm(MYSQL_THD thd, SYS_VAR *, void *val_ptr, const void *save) {
  *static_cast<char **>(val_ptr) = *static_cast<char *const *>(save);
  restart_engine(thd, "pool_prewarm");
}

void update_languages(MYSQL_THD, SYS_VAR *, void *val_ptr, const void *save) {
  char *s = *static_cast<char *const *>(save);
  *static_cast<char **>(val_ptr) = s;
  unsigned bits = 0;
  if (parse_languages(text(s), &bits)) g_language_bits = bits;
}

/** A registered variable, to unregister. */
struct Registered {
  const char *name;
  bool done;
};
Registered g_registered[] = {
    {"memory_max", false}, {"heap_max", false},    {"pool_size", false},
    {"max_workers", false}, {"spin_us", false},    {"interrupt_grace_ms", false},
    {"languages", false},  {"cgroup_base", false}, {"dir", false},
    {"java_home", false},  {"sql_result_max", false}, {"sql_row_max", false},
    {"sql_statements_max", false}, {"sql_chunk_rows", false},
    {"worker_cache_size", false}, {"worker_cache_timeout", false},
    {"package_dir", false}, {"pool_prewarm", false}, {"worker", false}};

void mark(const char *name) {
  for (auto &r : g_registered)
    if (strcmp(r.name, name) == 0) r.done = true;
}

template <typename Arg>
bool reg(const char *name, int flags, const char *comment,
         mysql_sys_var_check_func check, mysql_sys_var_update_func update,
         Arg *arg, void *variable) {
  if (mysql_service_component_sys_variable_register->register_variable(
          kName, name, flags, comment, check, update, arg, variable)) {
    log_error(std::string("Cannot register the system variable mle.") + name);
    return true;
  }
  mark(name);
  return false;
}

/** <plugin dir>/mle */
std::string default_dir() {
  char buffer[4096];
  void *value = buffer;
  size_t length = sizeof(buffer) - 1;
  if (mysql_service_mysql_system_variable_reader->get(
          nullptr, "GLOBAL", "mysql_server", "plugin_dir", &value, &length))
    return "mle";
  std::string dir(static_cast<const char *>(value), length);
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir + "/mle";
}

}  // namespace

bool register_settings() {
  INTEGRAL_CHECK_ARG(ulonglong) memory_max;
  memory_max.def_val = 2ULL << 30;
  memory_max.min_val = kMemoryMaxMin;
  memory_max.max_val = kMemoryMaxMax;
  memory_max.blk_sz = kMiB;
  if (reg("memory_max", PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED,
          "Memory limit of each MLE worker process in bytes (cgroup "
          "memory.max: JVM, GraalJS, GraalPy, numpy). Can be changed while no "
          "session holds a worker; restarts the engine.",
          check_shaping<ulonglong, kMemoryMax, kMemoryMaxMin, kMemoryMaxMax>,
          update_shaping<ulonglong, kMemoryMax>, &memory_max, &g_memory_max))
    return true;

  ulonglong_check_arg_s heap_max;
  heap_max.def_val = 512 * kMiB;
  heap_max.min_val = kHeapMaxMin;
  heap_max.max_val = kHeapMaxMax;
  heap_max.blk_sz = kMiB;
  if (reg("heap_max", PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED,
          "Java heap of each MLE worker in bytes (-Xmx). Can be changed while "
          "no session holds a worker; restarts the engine.",
          check_shaping<ulonglong, kHeapMax, kHeapMaxMin, kHeapMaxMax>,
          update_shaping<ulonglong, kHeapMax>, &heap_max, &g_heap_max))
    return true;

  INTEGRAL_CHECK_ARG(uint) pool_size;
  pool_size.def_val = 2;  // measured: first sessions of new accounts (12 §17)
  pool_size.min_val = 0;
  pool_size.max_val = kPoolSizeMax;
  pool_size.blk_sz = 0;
  if (reg("pool_size", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Started MLE workers kept ready for sessions (JVM with Java, "
          "JavaScript and Python loaded). Can be changed while no session "
          "holds a worker; restarts the engine.",
          check_shaping<uint, kPoolSize, 0, kPoolSizeMax>,
          update_shaping<uint, kPoolSize>, &pool_size, &g_pool_size))
    return true;

  STR_CHECK_ARG(pool_prewarm) pool_prewarm;
  pool_prewarm.def_val = const_cast<char *>("none");
  if (reg("pool_prewarm", PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC,
          "What pool workers start before a session needs them: none, python "
          "(GraalPy, ~1-5 s and ~195 MB per idle worker) or numpy (Python and "
          "numpy). Can be changed while no session holds a worker; restarts the "
          "engine.",
          check_prewarm, update_prewarm, &pool_prewarm, &g_pool_prewarm))
    return true;

  uint_check_arg_s max_workers;
  max_workers.def_val = 64;
  max_workers.min_val = 1;
  max_workers.max_val = 100000;
  max_workers.blk_sz = 0;
  if (reg("max_workers", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Most MLE workers held by sessions at a time (one per session and "
          "principal); beyond it calls fail with a concurrency limit error.",
          nullptr, nullptr, &max_workers, &g_max_workers))
    return true;

  // Reuse of workers by later sessions of the same principal (12 §17).
  uint_check_arg_s cache_size;
  cache_size.def_val = 4;
  cache_size.min_val = 0;
  cache_size.max_val = 1024;
  cache_size.blk_sz = 0;
  if (reg("worker_cache_size", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Most idle workers kept, after a reset, for later sessions of the "
          "same account (user@host); 0: a session's workers end with it.",
          nullptr, nullptr, &cache_size, &g_worker_cache_size))
    return true;

  uint_check_arg_s cache_timeout;
  cache_timeout.def_val = 60;
  cache_timeout.min_val = 1;
  cache_timeout.max_val = 86400;
  cache_timeout.blk_sz = 0;
  if (reg("worker_cache_timeout", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Seconds a kept idle worker waits for a session before it ends.",
          nullptr, nullptr, &cache_timeout, &g_worker_cache_timeout))
    return true;

  uint_check_arg_s spin_us;
  spin_us.def_val = 50;
  spin_us.min_val = 0;
  spin_us.max_val = 10000;
  spin_us.blk_sz = 0;
  if (reg("spin_us", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Microseconds a session spins waiting for its worker before it "
          "sleeps.",
          nullptr, nullptr, &spin_us, &g_spin_us))
    return true;

  uint_check_arg_s grace;
  grace.def_val = 1000;
  grace.min_val = 0;
  grace.max_val = 600000;
  grace.blk_sz = 0;
  if (reg("interrupt_grace_ms", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Milliseconds a worker has to stop a killed statement's routine "
          "before the worker is killed.",
          nullptr, nullptr, &grace, &g_interrupt_grace_ms))
    return true;

  // SQL from routines (12-phase5-sql-api.md §4, §11). Larger than Oracle's
  // 1 MB limits by default: rows with vectors are large already.
  ulonglong_check_arg_s result_max;
  result_max.def_val = 64 * kMiB;
  result_max.min_val = 1024;
  result_max.max_val = 1ULL << 40;
  result_max.blk_sz = 0;
  if (reg("sql_result_max", PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED,
          "Largest result in bytes of a statement a routine runs without "
          "parameters (the server buffers it whole). Oracle: 1 MB.",
          nullptr, nullptr, &result_max, &g_sql_result_max))
    return true;
  ulonglong_check_arg_s row_max;
  row_max.def_val = 16 * kMiB;
  row_max.min_val = 1024;
  row_max.max_val = 1ULL << 40;
  row_max.blk_sz = 0;
  if (reg("sql_row_max", PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED,
          "Largest row in bytes of a prepared statement a routine runs (rows "
          "are fetched one by one from a cursor). Oracle: 1 MB.",
          nullptr, nullptr, &row_max, &g_sql_row_max))
    return true;
  uint_check_arg_s statements_max;
  statements_max.def_val = 1024;
  statements_max.min_val = 1;
  statements_max.max_val = 1000000;
  statements_max.blk_sz = 0;
  if (reg("sql_statements_max", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Most statements a routine call keeps open (as Oracle: 1024).",
          nullptr, nullptr, &statements_max, &g_sql_statements_max))
    return true;
  uint_check_arg_s chunk_rows;
  chunk_rows.def_val = 1000;
  chunk_rows.min_val = 1;
  chunk_rows.max_val = 1000000;
  chunk_rows.blk_sz = 0;
  if (reg("sql_chunk_rows", PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED,
          "Rows of a statement's result sent to the worker at a time (a "
          "chunk also ends at about 768 KB).",
          nullptr, nullptr, &chunk_rows, &g_sql_chunk_rows))
    return true;

  STR_CHECK_ARG(languages) languages;
  languages.def_val = const_cast<char *>(kAllLanguageNames);
  if (reg("languages", PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC,
          "Languages MLE runs: a list of JAVASCRIPT, PYTHON, JAVA.",
          check_languages, update_languages, &languages, &g_languages))
    return true;
  /* A value from the command line or my.cnf is not checked by the server. */
  unsigned bits = 0;
  if (parse_languages(text(g_languages), &bits)) {
    g_language_bits = bits;
  } else {
    log_warning("mle.languages '" + text(g_languages) +
                "' is invalid; all languages are enabled.");
    g_language_bits = kAllLanguages;
  }

  STR_CHECK_ARG(cgroup_base) cgroup_base;
  cgroup_base.def_val = const_cast<char *>("AUTO");
  // Paths (cgroup_base, dir, java_home, package_dir): READONLY and not
  // persistable: what mysqld runs and what workers read is set where mysqld
  // is started (my.cnf), not by SET PERSIST_ONLY from SQL.
  if (reg("cgroup_base",
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY |
              PLUGIN_VAR_NOPERSIST,
          "cgroup v2 directory for the worker cgroups (memory and process "
          "limits): AUTO (the parent of mysqld's cgroup, if delegated), NONE "
          "or a path.",
          nullptr, nullptr, &cgroup_base, &g_cgroup_base))
    return true;

  g_default_dir = default_dir();
  STR_CHECK_ARG(dir) dir;
  dir.def_val = const_cast<char *>(g_default_dir.c_str());
  if (reg("dir", PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY |
              PLUGIN_VAR_NOPERSIST,
          "Directory of the MLE files (mle_launcher, mle_worker, jars, Python "
          "venv); default <plugin_dir>/mle.",
          nullptr, nullptr, &dir, &g_dir))
    return true;

  STR_CHECK_ARG(java_home) java_home;
  java_home.def_val = const_cast<char *>("");
  if (reg("java_home",
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY |
              PLUGIN_VAR_NOPERSIST,
          "GraalVM JDK 25 that runs the MLE workers; empty: <mle.dir>/jdk "
          "(package percona-mle-jdk), else the build's JDK.",
          nullptr, nullptr,
          &java_home, &g_java_home))
    return true;

  // Third-party packages (14 §3): installed by the DBA with mle_package on
  // the server host; routines can only read them.
  STR_CHECK_ARG(package_dir) package_dir;
  package_dir.def_val = const_cast<char *>("");
  if (reg("package_dir",
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY |
              PLUGIN_VAR_NOPERSIST,
          "Directory of third-party packages for routines (js/node_modules, "
          "python/site-packages, java/*.jar), installed by the DBA with "
          "mle_package; empty: <mle.dir>/packages.",
          nullptr, nullptr, &package_dir, &g_package_dir))
    return true;

  // The worker build (23): READONLY and not persistable, like the paths.
  STR_CHECK_ARG(worker) worker;
  worker.def_val = const_cast<char *>("AUTO");
  if (reg("worker",
          PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY |
              PLUGIN_VAR_NOPERSIST,
          "Which worker build runs routines: JVM (Java, JavaScript and "
          "Python), NATIVE (the Python-only native build in "
          "<mle.dir>/native, highly optimised for Python) or AUTO (the JVM "
          "build when it is installed, else the native one).",
          nullptr, nullptr, &worker, &g_worker))
    return true;
  return false;
}

void unregister_settings() {
  for (auto &r : g_registered) {
    if (!r.done) continue;
    if (!mysql_service_component_sys_variable_unregister->unregister_variable(
            kName, r.name))
      r.done = false;
  }
  g_languages = g_cgroup_base = g_dir = g_java_home = g_package_dir = g_pool_prewarm = g_worker = nullptr;
}

std::string package_dir() {
  const std::string dir = text(g_package_dir);  // READONLY: never replaced
  return dir.empty() ? text(g_dir) + "/packages" : dir;
}

Settings current_settings() {
  Settings s;
  s.memory_max = g_memory_max;
  s.heap_max = g_heap_max;
  s.pool_size = g_pool_size;
  s.cgroup_base = text(g_cgroup_base);
  s.dir = text(g_dir);
  s.java_home = text(g_java_home);
  if (s.java_home.empty()) {
    // The packaged runtime (21-packaging.md), else the JDK of the build.
    const std::string packaged = s.dir + "/jdk";
    s.java_home = access((packaged + "/lib/server/libjvm.so").c_str(), R_OK) == 0
                      ? packaged
                      : MLE_DEFAULT_JAVA_HOME;
  }
  s.package_dir = text(g_package_dir);
  s.pool_prewarm = text(g_pool_prewarm);
  for (char &c : s.pool_prewarm) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (s.pool_prewarm.empty()) s.pool_prewarm = "none";
  s.worker = text(g_worker);
  for (char &c : s.worker) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  if (s.worker.empty()) s.worker = "AUTO";
  if (s.package_dir.empty()) s.package_dir = s.dir + "/packages";
  return s;
}

unsigned enabled_languages() {
  return g_language_bits.load() & g_installed_bits.load();
}

void set_installed_languages(unsigned bits) { g_installed_bits = bits; }

uint32_t max_workers() { return g_max_workers; }
size_t worker_cache_size() { return g_worker_cache_size; }
uint32_t worker_cache_timeout() { return g_worker_cache_timeout; }
size_t sql_result_max() { return static_cast<size_t>(g_sql_result_max); }
size_t sql_row_max() { return static_cast<size_t>(g_sql_row_max); }
size_t sql_statements_max() { return g_sql_statements_max; }
uint32_t sql_chunk_rows() { return g_sql_chunk_rows; }
int spin_us() { return static_cast<int>(g_spin_us); }
int interrupt_grace_ms() { return static_cast<int>(g_interrupt_grace_ms); }

unsigned language_bit(std::string_view name) {
  auto is = [&](const char *n) {
    return name.size() == strlen(n) &&
           strncasecmp(name.data(), n, name.size()) == 0;
  };
  if (is("JAVASCRIPT")) return kJavaScript;
  if (is("PYTHON")) return kPython;
  if (is("JAVA")) return kJava;
  return 0;
}

bool parse_languages(std::string_view text, unsigned *bits) {
  *bits = 0;
  if (text.empty()) return true;  // no language
  while (true) {
    const size_t comma = text.find(',');
    std::string_view item = text.substr(0, comma);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    const unsigned bit = language_bit(item);
    if (bit == 0) return false;
    *bits |= bit;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return true;
}

std::string language_names(unsigned bits) {
  std::string names;
  auto add = [&](unsigned bit, const char *name) {
    if ((bits & bit) == 0) return;
    if (!names.empty()) names += ",";
    names += name;
  };
  add(kJavaScript, "JavaScript");
  add(kPython, "Python");
  add(kJava, "Java");
  return names;
}

}  // namespace mle_component
