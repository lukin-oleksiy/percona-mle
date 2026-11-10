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
  @file components/mle/component/component.cc

  component_mle: stored programs in LANGUAGE JAVASCRIPT (Oracle MySQL MLE
  compatible), PYTHON and JAVA, run in MLE worker processes (Phase 3) that
  a launcher started by this component provides. Next to Percona's V8
  LANGUAGE JS behind component_lang_router. Design: 11-phase4-component.md.

  P4-W2, this skeleton: the language services (routine handles counted, so
  UNINSTALL is refused while sessions cache MLE routines), the engine
  started at INSTALL (INSTALL fails clearly if it cannot start), the mle.*
  sysvars and the mle_* status variables. Routines are accepted at CREATE;
  running them comes with P4-W3/W4.
*/

#define LOG_COMPONENT_TAG "mle"

#include <cstring>
#include <string>

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/bits/stored_program_bits.h>
#include <mysql/components/services/component_status_var_service.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/language_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_runtime_warning.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysqld_error.h>

#include "mle_component.h"

REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_warning);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);

/* Used by the LogComponentErr macro. */
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

namespace mle_component {

void log_info(const std::string &message) {
  if (log_bi == nullptr) return;
  LogComponentErr(INFORMATION_LEVEL, ER_LANGUAGE_COMPONENT_INFO,
                  message.c_str());
}

void log_warning(const std::string &message) {
  if (log_bi == nullptr) return;
  LogComponentErr(WARNING_LEVEL, ER_LANGUAGE_COMPONENT_WARNING,
                  message.c_str());
}

void log_error(const std::string &message) {
  if (log_bi == nullptr) return;
  LogComponentErr(ERROR_LEVEL, ER_LANGUAGE_COMPONENT_ERROR, message.c_str());
}

namespace {

/** What the component keeps for a stored program cached in a session
    (the SQL layer's external_program_handle). */
struct Routine {
  unsigned language = 0;
  uint16_t type = 0;
  std::string qualified_name;
  std::string body;
};

bool is_function(const Routine &r) {
  return r.type == MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION;
}

void report(const std::string &message) {
  my_error(ER_LANGUAGE_COMPONENT, MYF(0), message.c_str());
}

std::string get_string(stored_program_handle sp, const char *key) {
  mysql_cstring_with_length value{nullptr, 0};
  if (mysql_service_mysql_stored_program_metadata_query->get(sp, key, &value) ||
      value.str == nullptr)
    return "";
  return {value.str, value.length};
}

class Services {
 public:
  /* external_program_capability_query */
  static DEFINE_BOOL_METHOD(get, (const char *capability, char *property,
                                  void *value)) {
    if (strcmp(capability, "supports_language") != 0) {
      report("Unknown capability request from the mle component.");
      return true;
    }
    *static_cast<bool *>(value) =
        (language_bit(property) & enabled_languages()) != 0;
    return false;
  }

  /* external_program_execution */
  static DEFINE_BOOL_METHOD(init, (stored_program_handle sp,
                                   stored_program_statement_handle,
                                   external_program_handle *lang_sp)) {
    const std::string language = get_string(sp, "sp_language");
    const unsigned bit = language_bit(language);
    if ((bit & enabled_languages()) == 0) {
      my_error(ER_LANGUAGE_COMPONENT_UNSUPPORTED_LANGUAGE, MYF(0),
               language.c_str());
      return true;
    }
    uint16_t type = 0;
    if (mysql_service_mysql_stored_program_metadata_query->get(sp, "sp_type",
                                                               &type))
      return true;
    if (type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION &&
        type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE) {
      report("MLE> Only stored functions and procedures are supported.");
      return true;
    }
    auto *routine = new Routine();
    routine->language = bit;
    routine->type = type;
    routine->qualified_name = get_string(sp, "qualified_name");
    routine->body = get_string(sp, "sp_body");
    (is_function(*routine) ? g_counters.functions : g_counters.procedures)++;
    raise_max(g_counters.program_bytes_max,
              static_cast<long>(routine->body.size()));
    *lang_sp = reinterpret_cast<external_program_handle>(routine);
    return false;
  }

  static DEFINE_BOOL_METHOD(deinit, (MYSQL_THD, external_program_handle lang_sp,
                                     stored_program_handle)) {
    if (lang_sp == nullptr) return false;
    auto *routine = reinterpret_cast<Routine *>(lang_sp);
    (is_function(*routine) ? g_counters.functions : g_counters.procedures)--;
    delete routine;
    return false;
  }

  /** CREATE, and when a routine enters a session's SP cache. The worker
      checks the syntax from P4-W4 on. */
  static DEFINE_BOOL_METHOD(parse, (external_program_handle,
                                    stored_program_statement_handle)) {
    return false;
  }

  static DEFINE_BOOL_METHOD(execute, (external_program_handle lang_sp,
                                      stored_program_statement_handle)) {
    auto *routine = reinterpret_cast<Routine *>(lang_sp);
    const EngineState st = engine_state(false);
    if (!st.active) {
      report("MLE> The MLE engine is not running: " +
             (st.error.empty() ? std::string("stopped") : st.error));
      return true;
    }
    report("MLE> " + routine->qualified_name +
           ": running MLE routines is not implemented yet.");
    return true;
  }
};

/** Starts the engine at INSTALL; a failure fails the INSTALL. */
bool start_engine() {
  std::string error, info;
  if (!engine_start(current_settings(), &error, &info)) {
    log_error("Cannot install: " + error);
    report("MLE: cannot install the component: " + error);
    return true;
  }
  log_info(info);
  return false;
}

mysql_service_status_t init() {
  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;
  if (register_settings() || register_status() || start_engine()) {
    engine_stop();
    unregister_status();
    unregister_settings();
    log_bi = nullptr;
    log_bs = nullptr;
    return 1;
  }
  return 0;
}

mysql_service_status_t deinit() {
  /* Routine handles live in the sessions' SP caches until the sessions end
     (or the routines are dropped or altered). No new one can appear here:
     the server does not unload a component whose services are in use. */
  if (g_counters.functions + g_counters.procedures > 0) {
    my_error(ER_LANGUAGE_COMPONENT_CANNOT_UNINSTALL, MYF(0));
    return 1;
  }
  engine_stop();
  unregister_status();
  unregister_settings();
  log_info("Engine stopped; component uninstalled.");
  log_bi = nullptr;
  log_bs = nullptr;
  return 0;
}

}  // namespace
}  // namespace mle_component

using mle_component::Services;

// clang-format off
BEGIN_SERVICE_IMPLEMENTATION(mle, external_program_capability_query)
  Services::get,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(mle, external_program_execution)
  Services::init,
  Services::deinit,
  Services::parse,
  Services::execute,
END_SERVICE_IMPLEMENTATION();

BEGIN_COMPONENT_PROVIDES(mle)
  PROVIDES_SERVICE(mle, external_program_capability_query),
  PROVIDES_SERVICE(mle, external_program_execution),
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(mle)
  REQUIRES_SERVICE(mysql_runtime_error),
  REQUIRES_SERVICE(mysql_runtime_warning),
  REQUIRES_SERVICE(mysql_stored_program_metadata_query),
  REQUIRES_SERVICE(component_sys_variable_register),
  REQUIRES_SERVICE(component_sys_variable_unregister),
  REQUIRES_SERVICE(mysql_system_variable_reader),
  REQUIRES_SERVICE(status_variable_registration),
  REQUIRES_SERVICE(log_builtins),
  REQUIRES_SERVICE(log_builtins_string),
END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(mle)
  METADATA("mysql.author", "Percona Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();

DECLARE_COMPONENT(mle, "mle")
  mle_component::init,
  mle_component::deinit,
END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS
  &COMPONENT_REF(mle)
END_DECLARE_LIBRARY_COMPONENTS
// clang-format on
