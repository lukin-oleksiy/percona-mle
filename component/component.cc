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

  Parts: the language services (here), routines (routine.cc: metadata,
  arguments, results), sessions (session.cc: a worker per session and
  principal, PREPARE/CALL/DROP, errors), the engine started at INSTALL
  (engine.cc), the mle.* sysvars (settings.cc) and the mle_* status
  variables (status.cc). UNINSTALL is refused while sessions that used MLE
  are connected.
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
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_string.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/components/services/dynamic_privilege.h>
#include <mysql/components/services/mysql_thd_attributes.h>
#include <mysql/components/services/mysql_thd_kill_handler.h>
#include <mysql/components/services/udf_metadata.h>
#include <mysql/components/services/udf_registration.h>
#include <mysql/components/services/mysql_thd_store_service.h>
#include <mysql/components/services/security_context.h>
#include <mysqld_error.h>

#include "mle_component.h"
#include "routine.h"
#include "session.h"

REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_warning);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_argument_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_int);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_unsigned_int);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_float);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_string_charset);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_null);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_int);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_unsigned_int);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_float);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_string_charset);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_null);
REQUIRES_SERVICE_PLACEHOLDER(mysql_charset);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_store);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_attributes);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_kill_handler);
REQUIRES_SERVICE_PLACEHOLDER(dynamic_privilege_register);
REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
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

void report(const std::string &message) {
  my_error(ER_LANGUAGE_COMPONENT, MYF(0), message.c_str());
}

Routine *routine_of(external_program_handle h) {
  return reinterpret_cast<Routine *>(h);
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

  /** A routine enters a session's routine cache (or CREATE checks it):
      metadata, parameter names and types are checked here. */
  static DEFINE_BOOL_METHOD(init, (stored_program_handle sp,
                                   stored_program_statement_handle,
                                   external_program_handle *lang_sp)) {
    Routine *routine = Routine::create(sp);
    if (routine == nullptr) return true;
    (routine->is_function() ? g_counters.functions : g_counters.procedures)++;
    raise_max(g_counters.program_bytes_max,
              static_cast<long>(routine->prepare.source.size()));
    *lang_sp = reinterpret_cast<external_program_handle>(routine);
    return false;
  }

  /** The routine leaves the cache: the session's workers drop it. thd is
      the session that cached it. */
  static DEFINE_BOOL_METHOD(deinit, (MYSQL_THD thd, external_program_handle lang_sp,
                                     stored_program_handle)) {
    if (lang_sp == nullptr) return false;
    Routine *routine = routine_of(lang_sp);
    // The handle CREATE used may come without a THD: it is the current one.
    Session *session = thd != nullptr ? Session::of(thd) : Session::current(false);
    if (session != nullptr) session->forget(*routine);
    (routine->is_function() ? g_counters.functions : g_counters.procedures)--;
    Routine::destroy(routine);
    return false;
  }

  /** CREATE, and when a routine enters the cache: the worker of the
      current principal compiles it (syntax errors fail the CREATE). */
  static DEFINE_BOOL_METHOD(parse, (external_program_handle lang_sp,
                                    stored_program_statement_handle)) {
    Session *session = Session::current(true);
    if (session == nullptr) {
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return true;
    }
    return !session->parse(*routine_of(lang_sp));
  }

  static DEFINE_BOOL_METHOD(execute, (external_program_handle lang_sp,
                                      stored_program_statement_handle)) {
    Session *session = Session::current(true);
    if (session == nullptr) {
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return true;
    }
    return !session->execute(*routine_of(lang_sp));
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
  if (register_settings() || register_status() || Session::register_slot() ||
      register_privileges() || register_udfs() || start_engine()) {
    engine_stop();
    (void)unregister_udfs();
    unregister_privileges();
    Session::unregister_slot();
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
  /* Sessions that used MLE keep their state in a thd_store slot, which
     the server does not free once the slot is unregistered: refuse until
     they disconnect (as js_lang). */
  if (g_counters.functions + g_counters.procedures > 0 || Session::count() > 0) {
    my_error(ER_LANGUAGE_COMPONENT_CANNOT_UNINSTALL, MYF(0));
    return 1;
  }
  /* A UDF that is running keeps its registration: refuse, try later. */
  if (unregister_udfs()) {
    my_error(ER_LANGUAGE_COMPONENT_CANNOT_UNINSTALL, MYF(0));
    return 1;
  }
  engine_stop();
  unregister_privileges();
  Session::unregister_slot();
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
  REQUIRES_SERVICE(mysql_stored_program_argument_metadata_query),
  REQUIRES_SERVICE(mysql_stored_program_return_metadata_query),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_int),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_unsigned_int),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_float),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_string),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_string_charset),
  REQUIRES_SERVICE(mysql_stored_program_runtime_argument_null),
  REQUIRES_SERVICE(mysql_stored_program_return_value_int),
  REQUIRES_SERVICE(mysql_stored_program_return_value_unsigned_int),
  REQUIRES_SERVICE(mysql_stored_program_return_value_float),
  REQUIRES_SERVICE(mysql_stored_program_return_value_string),
  REQUIRES_SERVICE(mysql_stored_program_return_value_string_charset),
  REQUIRES_SERVICE(mysql_stored_program_return_value_null),
  REQUIRES_SERVICE(mysql_charset),
  REQUIRES_SERVICE(mysql_current_thread_reader),
  REQUIRES_SERVICE(mysql_thd_store),
  REQUIRES_SERVICE(mysql_thd_attributes),
  REQUIRES_SERVICE(mysql_thd_security_context),
  REQUIRES_SERVICE(mysql_security_context_options),
  REQUIRES_SERVICE(mysql_thd_kill_handler),
  REQUIRES_SERVICE(dynamic_privilege_register),
  REQUIRES_SERVICE(global_grants_check),
  REQUIRES_SERVICE(udf_registration),
  REQUIRES_SERVICE(mysql_udf_metadata),
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
