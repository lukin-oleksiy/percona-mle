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
  @file component/status.cc

  The mle_* status variables: Oracle's names (05 §7), with our meaning
  where the engine differs (11-phase4-component.md §6a), and ours.

  Oracle's:
    mle_status               Active (engine running; from INSTALL, unlike
                             Oracle's Inactive until first use) / Inactive
    mle_heap_status          Allocated (worker processes exist) / Not
                             Allocated
    mle_languages_supported  JavaScript,Python,Java (mle.languages)
    mle_memory_used          % of the summed worker memory limits in use
    mle_oom_errors           workers that ran out of memory
    mle_session_resets       mle_session_reset() calls
    mle_sessions[_max]       sessions with a worker (now, most at a time)
    mle_stored_functions, mle_stored_procedures, mle_stored_programs
                             MLE stored programs cached in sessions
    mle_stored_program_bytes_max  largest stored program body
    mle_stored_program_sql_max    0 (SQL API: Phase 5)
    mle_threads[_max]        threads running MLE routines (now, most)
  Ours:
    mle_workers              workers held by sessions
    mle_workers_spawned      worker processes started by the launcher
    mle_worker_failures      workers that died, were killed or misbehaved
    mle_pool_idle            started workers waiting in the pool
    mle_workers_cached       reset workers kept for sessions of their account
    mle_worker_reuses        sessions that got a kept worker
    mle_cgroup               ON if workers run in their own cgroups
*/

#include <algorithm>
#include <cstring>
#include <string>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/component_status_var_service.h>
#include <mysql/status_var.h>

#include "mle_component.h"

extern REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);

namespace mle_component {

Counters g_counters;

namespace {

int show_text(SHOW_VAR *var, char *buff, const std::string &text) {
  var->type = SHOW_CHAR;
  var->value = buff;
  const size_t n = std::min(text.size(), size_t{SHOW_VAR_FUNC_BUFF_SIZE - 1});
  memcpy(buff, text.data(), n);
  buff[n] = '\0';
  return 0;
}

int show_number(SHOW_VAR *var, char *buff, long long value) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<long long *>(buff) = value;
  return 0;
}

template <std::atomic<long> Counters::*Member>
int show_counter(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, (g_counters.*Member).load());
}

int show_status(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_text(var, buff,
                   engine_state(false).active ? "Active" : "Inactive");
}

int show_heap_status(MYSQL_THD, SHOW_VAR *var, char *buff) {
  const EngineState st = engine_state(true);
  const bool allocated =
      st.active && (st.pool_idle > 0 || engine_workers_in_use() > 0);
  return show_text(var, buff, allocated ? "Allocated" : "Not Allocated");
}

int show_languages(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, language_names(enabled_languages()));
}

int show_memory_used(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, engine_memory_used_percent());
}

int show_programs(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff,
                     g_counters.functions.load() + g_counters.procedures.load());
}

int show_zero(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, 0);
}

int show_workers(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, engine_workers_in_use());
}

int show_spawned(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, static_cast<long long>(engine_state(true).spawned));
}

int show_pool_idle(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, engine_state(true).pool_idle);
}

int show_workers_cached(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, engine_state(false).cached);
}

int show_worker_reuses(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, static_cast<long long>(engine_state(false).cache_hits));
}

int show_cgroup(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, engine_state(false).cgroup ? "ON" : "OFF");
}

#define MLE_STATUS(name, func)                                            \
  {                                                                       \
    name, reinterpret_cast<char *>(func), SHOW_FUNC, SHOW_SCOPE_GLOBAL \
  }

SHOW_VAR g_status_vars[] = {
    MLE_STATUS("mle_heap_status", &show_heap_status),
    MLE_STATUS("mle_languages_supported", &show_languages),
    MLE_STATUS("mle_memory_used", &show_memory_used),
    MLE_STATUS("mle_oom_errors", &show_counter<&Counters::oom_errors>),
    MLE_STATUS("mle_session_resets", &show_counter<&Counters::session_resets>),
    MLE_STATUS("mle_sessions", &show_counter<&Counters::sessions>),
    MLE_STATUS("mle_sessions_max", &show_counter<&Counters::sessions_max>),
    MLE_STATUS("mle_status", &show_status),
    MLE_STATUS("mle_stored_functions", &show_counter<&Counters::functions>),
    MLE_STATUS("mle_stored_procedures", &show_counter<&Counters::procedures>),
    MLE_STATUS("mle_stored_programs", &show_programs),
    MLE_STATUS("mle_stored_program_bytes_max",
               &show_counter<&Counters::program_bytes_max>),
    MLE_STATUS("mle_stored_program_sql_max", &show_zero),
    MLE_STATUS("mle_threads", &show_counter<&Counters::threads>),
    MLE_STATUS("mle_threads_max", &show_counter<&Counters::threads_max>),
    MLE_STATUS("mle_workers", &show_workers),
    MLE_STATUS("mle_workers_spawned", &show_spawned),
    MLE_STATUS("mle_worker_failures",
               &show_counter<&Counters::worker_failures>),
    MLE_STATUS("mle_pool_idle", &show_pool_idle),
    MLE_STATUS("mle_workers_cached", &show_workers_cached),
    MLE_STATUS("mle_worker_reuses", &show_worker_reuses),
    MLE_STATUS("mle_cgroup", &show_cgroup),
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

bool g_registered = false;

}  // namespace

bool register_status() {
  if (mysql_service_status_variable_registration->register_variable(
          g_status_vars))
    return true;
  g_registered = true;
  return false;
}

void unregister_status() {
  if (g_registered &&
      !mysql_service_status_variable_registration->unregister_variable(
          g_status_vars))
    g_registered = false;
}

}  // namespace mle_component
