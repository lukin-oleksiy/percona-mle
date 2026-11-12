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
  @file components/mle/component/mle_component.h

  What the parts of component_mle share: languages, counters, settings
  (sysvars, settings.cc), status variables (status.cc) and the error log.
*/

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "engine.h"

namespace mle_component {

/** Name of the component: the prefix of its sysvars (mle.memory_max). */
constexpr const char kName[] = "mle";

/** Languages, as bits of mle.languages. */
enum Language : unsigned {
  kJavaScript = 1,
  kPython = 2,
  kJava = 4,
  kAllLanguages = 7
};

/** Bit of a LANGUAGE name (case-insensitive), 0 if it is not ours. */
unsigned language_bit(std::string_view name);

/** "JAVASCRIPT, python" -> bits. False on an unknown name. */
bool parse_languages(std::string_view text, unsigned *bits);

/** Display names, e.g. "JavaScript,Python,Java" (Oracle's
    mle_languages_supported spelling). */
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
/** mle.max_workers, mle.spin_us, mle.interrupt_grace_ms. */
uint32_t max_workers();
/** An account as 'user'@'host' (quotes doubled): the key of workers. */
std::string account_key(std::string_view user, std::string_view host);
/** mysqld's plugin_dir and datadir: never readable by workers (keyring
    component configuration, data files). */
std::vector<std::string> server_private_paths();
/** mle.package_dir (or its default); safe without locks (READONLY). */
std::string package_dir();
/** Whether the session's account has a global (dynamic) privilege. */
bool has_global_privilege(MYSQL_THD thd, const char *privilege);
/** mle.worker_cache_size, mle.worker_cache_timeout (seconds). */
size_t worker_cache_size();
uint32_t worker_cache_timeout();
int spin_us();
int interrupt_grace_ms();
/** SQL from routines (sql.cc): mle.sql_result_max, mle.sql_row_max,
    mle.sql_statements_max, mle.sql_chunk_rows. */
size_t sql_result_max();
size_t sql_row_max();
size_t sql_statements_max();
uint32_t sql_chunk_rows();

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
