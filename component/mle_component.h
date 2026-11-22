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

#ifndef COMPONENTS_MLE_COMPONENT_MLE_COMPONENT_H
#define COMPONENTS_MLE_COMPONENT_MLE_COMPONENT_H

/**
  @file component/mle_component.h

  What the parts of component_mle share: languages, counters, settings
  (sysvars, settings.cc), status variables (status.cc) and the error log.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "accounts.hpp"
#include "engine.h"

namespace mle_component {

/** Name of the component: the prefix of its sysvars (mle.memory_max). */
constexpr const char kName[] = "mle";

/** Languages, as bits of mle.languages: a LANGUAGE name each. The worker
    kind runs it (exp design note 63): JAVASCRIPT, PYTHON and JAVA on the JVM
    worker (GraalJS, GraalPy, Java), JS on the V8 worker, CPYTHON on the
    CPython worker. */
enum Language : unsigned {
  kJavaScript = 1,
  kPython = 2,
  kJava = 4,
  kJs = 8,
  kCPython = 16,
  kAllLanguages = 31
};

/** The kind that runs a language bit. */
constexpr WorkerKind kind_of(unsigned bit) {
  return bit == kJs ? WorkerKind::kV8 : bit == kCPython ? WorkerKind::kCPython : WorkerKind::kJvm;
}

/** The language a bit is written in, as the workers and the rules for names
    and values see it: JS is JavaScript (Oracle's), CPYTHON is Python. */
constexpr unsigned family(unsigned bit) {
  return bit == kJs ? kJavaScript : bit == kCPython ? kPython : bit;
}

/** Language bits a kind runs. */
constexpr unsigned kind_languages(WorkerKind kind) {
  return kind == WorkerKind::kV8 ? unsigned{kJs}
         : kind == WorkerKind::kCPython ? unsigned{kCPython}
                                  : unsigned{kJavaScript | kPython | kJava};
}

/** Bit of a LANGUAGE name (case-insensitive), 0 if it is not ours. */
unsigned language_bit(std::string_view name);

/** The LANGUAGE name of a bit ("JAVASCRIPT", "JS", "CPYTHON", ...). */
const char *language_sql_name(unsigned bit);

/** "JAVASCRIPT, python" -> bits. False on an unknown name. */
bool parse_languages(std::string_view text, unsigned *bits);

/** Parses mle.pool_prewarm: "none", or a comma list of python, numpy and
    vectors (any case, spaces around items). On success *normalized is
    "none" or the items lowercase, in that order, without repeats. */
bool parse_prewarm(std::string_view text, std::string *normalized);

/** Display names, e.g. "JavaScript,Python,Java,JS,CPython" (Oracle's
    mle_languages_supported spelling for its own). */
std::string language_names(unsigned bits);

/** Counters behind the status variables. */
struct Counters {
  /** Routine handles (stored programs cached in sessions). */
  std::atomic<long> functions{0};
  std::atomic<long> procedures{0};
  /** Largest routine body seen. */
  std::atomic<long> program_bytes_max{0};
  /** Sessions with a worker, and the most at a time (W4). */
  std::atomic<long> sessions{0};
  std::atomic<long> sessions_max{0};
  /** Threads running a routine, and the most at a time (W4). */
  std::atomic<long> threads{0};
  std::atomic<long> threads_max{0};
  std::atomic<long> session_resets{0};
  std::atomic<long> oom_errors{0};
  /** Workers lost: died, killed or broke the protocol (W5). */
  std::atomic<long> worker_failures{0};
  /** GCs the workers' memory watchers forced (exp note 30). */
  std::atomic<long> memory_gcs{0};
};
extern Counters g_counters;

/** Raises max to at least value. */
inline void raise_max(std::atomic<long> &max, long value) {
  long seen = max.load();
  while (value > seen && !max.compare_exchange_weak(seen, value)) {
  }
}

/* settings.cc: the mle.* system variables. */
bool register_settings();
void unregister_settings();
/** The engine settings from the sysvars. */
Settings current_settings();
/** Languages installed (engine start: Python is an optional package). */
void set_installed_languages(unsigned bits);
/** Bits of mle.languages that are installed. */
unsigned enabled_languages();

/** mle.console = WARNINGS in this session (exp note 39). */
bool console_warnings(MYSQL_THD thd);
/** mle.max_workers, mle.spin_us, mle.interrupt_grace_ms. */
uint32_t max_workers();
/** mysqld's plugin_dir and datadir: never readable by workers (keyring
    component configuration, data files). */
bool server_private_paths(std::vector<std::string> *paths);
/** mle.package_dir (or its default); safe without locks (READONLY). */
std::string package_dir();
/** Whether the session's account has a global (dynamic) privilege. */
bool has_global_privilege(MYSQL_THD thd, const char *privilege);
/** mle.worker_cache_size, mle.worker_cache_timeout (seconds). */
size_t worker_cache_size();
uint32_t worker_cache_timeout();
int spin_us();
int interrupt_grace_ms();
/** Changes when spin_us() or interrupt_grace_ms() may have changed. */
uint64_t timing_generation();
/** SQL from routines (sql.cc): mle.sql_result_max, mle.sql_row_max,
    mle.sql_statements_max, mle.sql_chunk_rows. */
size_t sql_result_max();
size_t sql_row_max();
size_t sql_statements_max();
/** mle.sql_call_results_max (0: four times mle.sql_result_max). */
size_t sql_call_results_max();
uint32_t sql_chunk_rows();
/** mle.sql_exec_time: whether a statement's execution time is measured. */
bool sql_exec_time();

/* session.cc: the library epoch (protocol §9.10). A number that only grows;
   every PREPARE and CALL carries its value. It changes after a library, a
   routine's USING clause or the right to use a library may have changed and
   before the statement that made the change reports success. */
uint64_t library_epoch();
void library_epoch_changed();
/** A statement of a command ended (sql_command: the server's name of it, as
    its query events carry it, e.g. "create_library"): changes the epoch if
    that command can change a library, a routine or a right, and retires
    workers that may name an account the command created. A command is
    looked up by its name once; after that it is known by the address of
    the server's name for it. */
void statement_ended(const char *sql_command);

/* status.cc: the mle_* status variables. */
bool register_status();
void unregister_status();

/* udfs.cc: mle_session_state, mle_session_reset, mle_set_session_state. */
bool register_udfs();
bool unregister_udfs();

/* component.cc: the error log. */
void log_info(const std::string &message);
void log_warning(const std::string &message);
void log_error(const std::string &message);

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_MLE_COMPONENT_H
