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
  @file component/udfs.cc

  Oracle MLE's session UDFs (MySQL manual 26.10, 05 §7):
    mle_session_state(key)  is_active, stdout, stderr, stack_trace,
                            stored_functions, stored_procedures,
                            stored_programs; and (exp note 39) traceback,
                            last_output
    mle_session_reset([what])  'stdout', 'stderr', 'output', or no argument
                            for the whole session (workers, programs, stack
                            trace, settings)
    mle_set_session_state(json)  {"integer_type": ..., "decimal_type": ...}:
                            the session's settings integer_type and
                            decimal_type (protocol Appendix B.2); only
                            while no programs are cached, else 1123
                            "... Cannot set options of an active session.
                            Please reset the session first."
  MLE's own:
    mle_eval(language, code)  a snippet in the session's worker: what it
                            printed and the value of its last expression
                            (exp note 39); the language's CREATE privilege;
                            no SQL.
  Debug builds add mle_debug_worker_pid() for the failure tests.
*/

#include <strings.h>

#include <array>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/udf_metadata.h>
#include <mysql/components/services/udf_registration.h>
#include <mysqlpp/udf_context_charset_extension.hpp>
#include <mysqlpp/udf_error_reporter.hpp>
#include <mysqlpp/udf_registration.hpp>
#include <mysqlpp/udf_wrappers.hpp>

#include "engine.h"
#include "mle_component.h"
#include "session.h"

extern REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_udf_metadata);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);

namespace {

using mle_component::Session;

constexpr std::size_t kTextLength = 65535;

void utf8_result(mysqlpp::udf_context &ctx) {
  ctx.set_result_max_length(kTextLength);
  mysqlpp::udf_context_charset_extension charset_ext{
      mysql_service_mysql_udf_metadata};
  charset_ext.set_return_value_collation(ctx, "utf8mb4_0900_ai_ci");
}

/** A text argument arrives as utf8mb4 whatever the connection's character
    set (code and option names are sent to the workers as UTF-8). */
void utf8_arg(mysqlpp::udf_context &ctx, std::size_t index) {
  mysqlpp::udf_context_charset_extension charset_ext{
      mysql_service_mysql_udf_metadata};
  charset_ext.set_arg_value_charset(ctx, index, "utf8mb4");
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char &c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return r;
}

class mle_session_state_impl {
 public:
  explicit mle_session_state_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument("Wrong argument list: should be (key)");
    ctx.set_arg_type(0, STRING_RESULT);
    ctx.mark_arg_nullable(0, false);
    ctx.mark_result_nullable(true);
    ctx.mark_result_const(false);
    utf8_result(ctx);
  }
  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx) {
    if (ctx.is_arg_null(0)) return {};  // NULL for NULL
    const auto key = ctx.get_arg<STRING_RESULT>(0);
    return Session::state(Session::current(false),
                          lower(key));
  }
};

class mle_session_reset_impl {
 public:
  explicit mle_session_reset_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() > 1)
      throw std::invalid_argument(
          "Wrong argument list: should be ([stdout|stderr|output])");
    if (ctx.get_number_of_args() == 1) {
      ctx.set_arg_type(0, STRING_RESULT);
      ctx.mark_arg_nullable(0, false);
    }
    ctx.mark_result_nullable(false);
    ctx.mark_result_const(false);
    utf8_result(ctx);
  }
  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx) {
    Session *s = Session::current(false);
    if (ctx.get_number_of_args() == 0) {
      if (s != nullptr && !s->reset())
        throw std::invalid_argument(
            "mle_session_reset() cannot reset the session while one of its "
            "MLE routines runs");
    } else {
      if (ctx.is_arg_null(0))
        throw std::invalid_argument("the argument must be stdout, stderr or output");
      const std::string what = lower(ctx.get_arg<STRING_RESULT>(0));
      const bool out = what == "stdout" || what == "output";
      const bool err = what == "stderr" || what == "output";
      if (!out && !err)
        throw std::invalid_argument("the argument must be stdout, stderr or output");
      if (s != nullptr) s->reset_output(out, err);
    }
    return std::string("The session state is successfully reset.");
  }
};

/**
  The options of mle_set_session_state(): a flat JSON object of strings,
  {"key": "value", ...}, and nothing else (independent review C94: text that
  is not such an object used to pass as "no options" and reset both).
*/
std::vector<std::pair<std::string, std::string>> json_options(
    const std::string &json) {
  std::vector<std::pair<std::string, std::string>> options;
  size_t i = 0;
  const auto fail = [] {
    throw std::invalid_argument(
        "the argument must be a JSON object of options, such as "
        "{\"integer_type\": \"BIGINT\"}");
  };
  const auto space = [&] {
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) i++;
  };
  const auto expect = [&](char c) {
    space();
    if (i >= json.size() || json[i] != c) fail();
    i++;
  };
  const auto string = [&] {
    expect('"');
    std::string text;
    for (; i < json.size() && json[i] != '"'; i++) {
      if (json[i] == '\\' && ++i >= json.size()) fail();
      text.push_back(json[i]);
    }
    if (i >= json.size()) fail();
    i++;
    return text;
  };
  expect('{');
  space();
  if (i < json.size() && json[i] == '}') {
    i++;
  } else {
    while (true) {
      std::string key = string();
      expect(':');
      space();
      if (i >= json.size() || json[i] != '"')
        throw std::invalid_argument("the value of " + key + " must be a string");
      options.emplace_back(std::move(key), string());
      space();
      if (i < json.size() && json[i] == ',') {
        i++;
        continue;
      }
      expect('}');
      break;
    }
  }
  space();
  if (i != json.size()) fail();
  return options;
}

/** mle_packages(): the DBA's package manifest (14 §3) as JSON text. */
class mle_packages_impl {
 public:
  explicit mle_packages_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 0)
      throw std::invalid_argument("Wrong argument list: should be ()");
    ctx.mark_result_nullable(false);
    ctx.mark_result_const(false);
    utf8_result(ctx);
  }
  mysqlpp::udf_result_t<STRING_RESULT> calculate(const mysqlpp::udf_context &) {
    // Only READONLY settings here: other string sysvars may be freed by a
    // concurrent SET GLOBAL.
    std::ifstream in(mle_component::package_dir() + "/packages.json");
    if (!in) return std::string("[]");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
};

/** mle_packages_refresh(): new workers for newly installed packages. */
class mle_packages_refresh_impl {
 public:
  explicit mle_packages_refresh_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 0)
      throw std::invalid_argument("Wrong argument list: should be ()");
    ctx.mark_result_nullable(false);
    ctx.mark_result_const(false);
    utf8_result(ctx);
  }
  mysqlpp::udf_result_t<STRING_RESULT> calculate(const mysqlpp::udf_context &) {
    MYSQL_THD thd = nullptr;
    if (mysql_service_mysql_current_thread_reader->get(&thd) ||
        !mle_component::has_global_privilege(thd, "SYSTEM_VARIABLES_ADMIN"))
      throw std::invalid_argument(
          "mle_packages_refresh() needs the SYSTEM_VARIABLES_ADMIN privilege");
    std::string error;
    if (!mle_component::engine_refresh(&error))
      throw std::invalid_argument("cannot refresh: " + error);
    return std::string("New MLE workers will see the installed packages.");
  }
};

class mle_set_session_state_impl {
 public:
  explicit mle_set_session_state_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument("Wrong argument list: should be (json)");
    ctx.set_arg_type(0, STRING_RESULT);
    ctx.mark_arg_nullable(0, false);
    utf8_arg(ctx, 0);
    ctx.mark_result_nullable(false);
    ctx.mark_result_const(false);
    // Oracle reports this when the function is set up (error 1123). The
    // ids of mle_eval() code and of routines that are gone are dropped
    // first (protocol Appendix A).
    if (Session *s = Session::current(false);
        s != nullptr && !s->options_replaceable())
      throw std::invalid_argument(
          "Cannot set options of an active session. Please reset the "
          "session first.");
  }
  mysqlpp::udf_result_t<INT_RESULT> calculate(const mysqlpp::udf_context &ctx) {
    if (ctx.is_arg_null(0))
      throw std::invalid_argument("the argument must not be NULL");
    const std::string json(ctx.get_arg<STRING_RESULT>(0));
    Session *s = Session::current(true);
    if (s == nullptr) throw std::invalid_argument("no MLE session");
    // An option that is not given keeps its value. The settings go to the
    // workers by name and value; the default of each is sent as its absence.
    std::vector<std::pair<std::string, std::string>> settings;
    for (const auto &[key, value] : json_options(json)) {
      const std::string v = lower(value);
      if (key == "integer_type") {
        if (v != "unsafe_string" && v != "unsafe_bigint" && v != "string" &&
            v != "bigint")
          throw std::invalid_argument("invalid integer_type '" + value + "'");
        settings.emplace_back("integer_type", v == "unsafe_string" ? "" : v);
      } else if (key == "decimal_type") {
        if (v != "string" && v != "number")
          throw std::invalid_argument("invalid decimal_type '" + value + "'");
        settings.emplace_back("decimal_type", v == "string" ? "" : v);
      } else {
        // An unknown key (e.g. integerType, the SQL API's spelling) would
        // otherwise be ignored without a word.
        throw std::invalid_argument("unknown option '" + key +
                                    "': the options are integer_type and "
                                    "decimal_type");
      }
    }
    if (!s->set_settings(settings))
      throw std::invalid_argument(
          "Cannot set options of an active session. Please reset the "
          "session first.");
    return 1;
  }
};

class mle_eval_impl {
 public:
  explicit mle_eval_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 2)
      throw std::invalid_argument("Wrong argument list: should be (language, code)");
    ctx.set_arg_type(0, STRING_RESULT);
    ctx.set_arg_type(1, STRING_RESULT);
    ctx.mark_arg_nullable(0, false);
    ctx.mark_arg_nullable(1, false);
    utf8_arg(ctx, 0);
    utf8_arg(ctx, 1);
    ctx.mark_result_nullable(true);
    ctx.mark_result_const(false);
    utf8_result(ctx);
  }
  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &ctx) {
    if (ctx.is_arg_null(0) || ctx.is_arg_null(1)) return {};  // NULL for NULL
    const auto language = ctx.get_arg<STRING_RESULT>(0);
    const auto code = ctx.get_arg<STRING_RESULT>(1);
    const unsigned bit = mle_component::language_bit(std::string(language));
    if (bit == 0)
      throw std::invalid_argument("the language must be JAVASCRIPT, PYTHON or JAVA");
    if ((mle_component::enabled_languages() & bit) == 0)
      throw std::invalid_argument("the language is not enabled (mle.languages)");
    Session *s = Session::current(true);
    if (s == nullptr) throw std::invalid_argument("no MLE session");
    // An error was reported (my_error): the statement fails with it.
    return s->eval(bit, std::string(code));
  }
};

#ifndef NDEBUG
/** Debug builds: the pid of the session's worker for the current user
    (failure tests kill it from outside): of the kind named ('jvm',
    'cpython', 'v8'), or without an argument the JVM worker's, else any.
    NULL without such a worker (MVP review TEST-4). */
class mle_debug_worker_pid_impl {
 public:
  explicit mle_debug_worker_pid_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() > 1)
      throw std::invalid_argument("Wrong argument list: should be ([kind])");
    if (ctx.get_number_of_args() == 1) ctx.set_arg_type(0, STRING_RESULT);
    ctx.mark_result_nullable(true);
    ctx.mark_result_const(false);
  }
  mysqlpp::udf_result_t<INT_RESULT> calculate(const mysqlpp::udf_context &ctx) {
    using mle_component::WorkerKind;
    std::optional<WorkerKind> kind;
    if (ctx.get_number_of_args() == 1) {
      const auto name = ctx.get_arg<STRING_RESULT>(0);
      for (const WorkerKind k : {WorkerKind::kJvm, WorkerKind::kCPython, WorkerKind::kV8})
        if (name == mle_component::kind_name(k)) kind = k;
      if (!kind) return std::nullopt;
    }
    Session *s = Session::current(false);
    const long pid = s == nullptr ? -1 : s->worker_pid(kind);
    if (pid < 0) return std::nullopt;
    return pid;
  }
};
#endif

}  // namespace

DECLARE_STRING_UDF_AUTO(mle_session_state)
DECLARE_STRING_UDF_AUTO(mle_session_reset)
DECLARE_INT_UDF_AUTO(mle_set_session_state)
DECLARE_STRING_UDF_AUTO(mle_packages)
DECLARE_STRING_UDF_AUTO(mle_packages_refresh)
DECLARE_STRING_UDF_AUTO(mle_eval)
#ifndef NDEBUG
DECLARE_INT_UDF_AUTO(mle_debug_worker_pid)
#endif

namespace {

std::array g_udfs{DECLARE_UDF_INFO_AUTO(mle_session_state),
                  DECLARE_UDF_INFO_AUTO(mle_session_reset),
                  DECLARE_UDF_INFO_AUTO(mle_packages),
                  DECLARE_UDF_INFO_AUTO(mle_packages_refresh),
                  DECLARE_UDF_INFO_AUTO(mle_set_session_state),
                  DECLARE_UDF_INFO_AUTO(mle_eval)
#ifndef NDEBUG
                      ,
                  DECLARE_UDF_INFO_AUTO(mle_debug_worker_pid)
#endif
};

mysqlpp::udf_bitset<std::tuple_size_v<decltype(g_udfs)>> g_registered;

}  // namespace

namespace mle_component {

namespace {
/** Errors of the UDF wrappers (ER_UDF_ERROR ...) go to the client. */
void udf_error(int error_id, myf flags, ...) {
  va_list args;
  va_start(args, flags);
  mysql_service_mysql_runtime_error->emit(error_id, flags, args);
  va_end(args);
}
}  // namespace

bool register_udfs() {
  mysqlpp::udf_error_reporter::instance() = &udf_error;
  mysqlpp::register_udfs(mysql_service_udf_registration, g_udfs, g_registered);
  return !g_registered.all();
}

bool unregister_udfs() {
  mysqlpp::unregister_udfs(mysql_service_udf_registration, g_udfs,
                           g_registered);
  return !g_registered.none();
}

}  // namespace mle_component
