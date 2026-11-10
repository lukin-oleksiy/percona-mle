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

#include "session.h"

#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/mysql_thd_attributes_bits.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/components/services/dynamic_privilege.h>
#include <mysql/components/services/mysql_thd_attributes.h>
#include <mysql/components/services/mysql_thd_kill_handler.h>
#include <mysql/components/services/mysql_thd_store_service.h>
#include <mysql/components/services/security_context.h>
#include <mysql/mysql_lex_string.h>
#include <mysqld_error.h>

#include "mle_component.h"

extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_store);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_attributes);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_system_variable_reader);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_kill_handler);
extern REQUIRES_SERVICE_PLACEHOLDER(dynamic_privilege_register);
extern REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);

namespace mle_component {

using mle::ResultMsg;
using mle::Status;
using mle::Value;

namespace {

mysql_thd_store_slot g_slot = nullptr;
std::atomic<long> g_sessions{0};

constexpr size_t kConsoleKeep = 64 * 1024;

/** The SQL layer's side of a call: SQL from routines (Phase 5) and KILL. */
class Hooks : public mle::CallHooks {
 public:
  explicit Hooks(MYSQL_THD thd, SqlRunner *sql = nullptr)
      : m_thd(thd), m_sql(sql) {}

  /** SQL from the routine (Phase 5): the call's statements (sql.cc). */
  void sql(const mle::SqlMsg &m, mle::SqlResultMsg *r) override {
    if (m_sql != nullptr) return m_sql->handle(m, r);
    sql_error(r, ER_LANGUAGE_COMPONENT_INTERNAL,
              "SQL is only possible while a routine runs");
  }

  /** KILL QUERY / KILL CONNECTION / max_execution_time. */
  bool killed() override {
    uint16_t status = STATUS_SESSION_OK;
    mysql_service_mysql_thd_attributes->get(m_thd, "thd_status", &status);
    return status != STATUS_SESSION_OK;
  }

 private:
  MYSQL_THD m_thd;
  SqlRunner *m_sql;
};

std::string session_time_zone(MYSQL_THD thd) {
  char buffer[256];
  void *value = buffer;
  size_t length = sizeof buffer - 1;
  if (mysql_service_mysql_system_variable_reader->get(
          thd, "SESSION", "mysql_server", "time_zone", &value, &length))
    return "+00:00";
  std::string zone(static_cast<const char *>(value), length);
  return zone == "SYSTEM" ? system_time_zone() : zone;
}

void append_capped(std::string *buffer, const std::string &more) {
  if (more.empty()) return;
  buffer->append(more);
  if (buffer->size() > kConsoleKeep)
    buffer->erase(0, buffer->size() - kConsoleKeep);
}

}  // namespace

/** KILL QUERY / KILL CONNECTION / max_execution_time interrupt the worker
    at once (the worker's killed() polling is the backstop). */
class Session::KillGuard {
 public:
  KillGuard(Session *s, mle::Worker *worker) : m_session(s) {
    const size_t n = s->m_running_n.load();
    m_pushed = n < kMaxNesting;
    if (m_pushed) {
      s->m_running[n].store(worker);
      s->m_running_n.store(n + 1);
    }
    if (n == 0) {
      // Fails if another component's handler is installed (e.g. a V8
      // routine called us): the workers' KILL polling still works.
      s->m_kill_installed = !mysql_service_mysql_thd_kill_handler->set(
          s->m_thd,
          [](MYSQL_THD, uint16_t, void *data) {
            auto *session = static_cast<Session *>(data);
            session->m_kills.fetch_add(1);
            const size_t count = session->m_running_n.load();
            for (size_t i = 0; i < count && i < kMaxNesting; i++)
              if (mle::Worker *w = session->m_running[i].load()) w->interrupt();
          },
          s);
    }
  }
  ~KillGuard() {
    if (m_pushed) {
      const size_t n = m_session->m_running_n.load() - 1;
      m_session->m_running[n].store(nullptr);
      m_session->m_running_n.store(n);
    }
    if (m_session->m_running_n.load() == 0 && m_session->m_kill_installed) {
      (void)mysql_service_mysql_thd_kill_handler->set(m_session->m_thd,
                                                      nullptr, nullptr);
      m_session->m_kill_installed = false;
    }
  }
  KillGuard(const KillGuard &) = delete;
  KillGuard &operator=(const KillGuard &) = delete;

 private:
  Session *m_session;
  bool m_pushed = false;
};

// ------------------------------------------------------------- lifetime

bool Session::register_slot() {
  auto free_session = [](void *resource) {
    delete static_cast<Session *>(resource);
    return 0;
  };
  return mysql_service_mysql_thd_store->register_slot("mle", free_session,
                                                      &g_slot);
}

void Session::unregister_slot() {
  if (g_slot == nullptr) return;
  (void)mysql_service_mysql_thd_store->unregister_slot(g_slot);
  g_slot = nullptr;
}

long Session::count() { return g_sessions.load(); }

Session *Session::of(MYSQL_THD thd) {
  if (g_slot == nullptr || thd == nullptr) return nullptr;
  return static_cast<Session *>(mysql_service_mysql_thd_store->get(thd, g_slot));
}

Session *Session::current(bool create) {
  MYSQL_THD thd = nullptr;
  if (mysql_service_mysql_current_thread_reader->get(&thd) || thd == nullptr)
    return nullptr;
  Session *s = of(thd);
  if (s != nullptr || !create) return s;
  s = new Session(thd);
  if (mysql_service_mysql_thd_store->set(thd, g_slot, s)) {
    delete s;
    return nullptr;
  }
  raise_max(g_counters.sessions_max, ++g_sessions);
  g_counters.sessions = g_sessions.load();
  return s;
}

Session::~Session() {
  for (auto &[principal, slot] : m_workers)
    engine_release_worker(std::move(slot.worker));
  g_counters.sessions = --g_sessions;
}

const std::string &Session::principal() const {
  Security_context_handle sctx = nullptr;
  MYSQL_LEX_CSTRING user{"", 0}, host{"", 0};
  if (mysql_service_mysql_thd_security_context->get(m_thd, &sctx) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_user",
                                                        &user) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_host",
                                                        &host)) {
    m_principal_sctx = nullptr;
    m_principal = "?";
    return m_principal;
  }
  // The same security context with the same account strings: the same
  // principal (the strings are the context's own buffers).
  if (sctx != m_principal_sctx || user.str != m_principal_user ||
      host.str != m_principal_host) {
    m_principal.assign(user.str, user.length);
    m_principal += '@';
    m_principal.append(host.str, host.length);
    m_principal_sctx = sctx;
    m_principal_user = user.str;
    m_principal_host = host.str;
  }
  return m_principal;
}

// ------------------------------------------------------------- workers

void Session::drop_worker(const std::string &principal) {
  auto it = m_workers.find(principal);
  if (it == m_workers.end()) return;
  engine_release_worker(std::move(it->second.worker));
  m_workers.erase(it);
}

Session::Slot *Session::worker_for(const std::string &principal,
                                   const Routine &routine) {
  auto it = m_workers.find(principal);
  if (it != m_workers.end()) {
    if (it->second.worker && it->second.worker->usable()) return &it->second;
    drop_worker(principal);  // died or retired: a fresh one below
  }

  if (!m_options_bound) {
    // Oracle: the session's time zone is bound at its first use (until
    // mle_session_reset()).
    m_options.time_zone = session_time_zone(m_thd);
    m_options_bound = true;
  }
  const std::string &zone = m_options.time_zone;
  if (zone.rfind("leap/", 0) == 0 || zone.rfind("right/", 0) == 0) {
    my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0), routine.display_language(),
             zone.c_str());
    return nullptr;
  }

  AcquireError why = AcquireError::kNone;
  std::string error;
  std::unique_ptr<mle::Worker> worker =
      engine_new_worker(max_workers(), spin_us(), interrupt_grace_ms(), &why,
                        &error);
  switch (why) {
    case AcquireError::kNone:
      break;
    case AcquireError::kLimit:
      my_error(ER_LANGUAGE_COMPONENT_CONCURRENCY_LIMIT, MYF(0),
               ("mle.max_workers = " + std::to_string(max_workers())).c_str());
      return nullptr;
    case AcquireError::kInactive:
      my_error(ER_LANGUAGE_COMPONENT, MYF(0),
               ("MLE> The MLE engine is not running: " + error).c_str());
      return nullptr;
    case AcquireError::kFailed:
      log_error("No worker for " + principal + ": " + error);
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return nullptr;
  }

  if (worker->options(m_options, &error) != Status::kOk) {
    engine_release_worker(std::move(worker));
    if (error.find("unknown time zone") != std::string::npos) {
      my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0),
               routine.display_language(), zone.c_str());
    } else {
      log_error("A new worker for " + principal + " failed: " + error);
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    }
    return nullptr;
  }
  Slot &slot = m_workers[principal];
  slot.worker = std::move(worker);
  return &slot;
}

bool Session::prepare_in(const std::string &principal, Slot &slot,
                         const Routine &routine) {
  if (slot.prepared.count(routine.id) != 0) return true;
  Hooks hooks(m_thd);
  ResultMsg result;
  std::string error;
  Status s;
  {
    const KillGuard kill(this, slot.worker.get());
    s = slot.worker->prepare(routine.prepare, hooks, &result, &error);
  }
  keep_console(result);
  if (s != Status::kOk) {
    report(principal, s, result, error, routine, nullptr);
    return false;
  }
  slot.prepared.insert(routine.id);
  m_programs[routine.id] = routine.sp_type;
  return true;
}

// ------------------------------------------------------------- requests

bool Session::parse(Routine &routine) {
  if (!check_create_privilege(m_thd, routine)) return false;
  const std::string who = principal();
  Slot *slot = worker_for(who, routine);
  return slot != nullptr && prepare_in(who, *slot, routine);
}

bool Session::execute(Routine &routine) {
  m_stack.clear();  // Oracle: the trace of the last call only
  std::vector<Value> args = std::move(m_args);  // reused; a nested call allocates
  if (!routine.read_args(&args)) return false;
  const std::string &who = principal();
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr || !prepare_in(who, *slot, routine)) return false;

  SqlRunner sql(m_thd);  // the call's statements; closed when it returns
  Hooks hooks(m_thd, &sql);
  ResultMsg result;
  std::string error;
  raise_max(g_counters.threads_max, ++g_counters.threads);
  Status s;
  const uint64_t kills = m_kills.load();
  {
    const KillGuard kill(this, slot->worker.get());
    s = slot->worker->call(routine.id, 0, args, hooks, &result, &error);
  }
  --g_counters.threads;
  keep_console(result);
  // A call killed meanwhile reports the KILL even if the routine then ended
  // normally (e.g. an inner SLEEP returned early and the worker finished
  // before the interrupt reached it).
  if (s == Status::kOk && (hooks.killed() || m_kills.load() != kills))
    s = Status::kInterrupted;
  const bool ok = s == Status::kOk;
  if (ok) sql.call_succeeded();
  else report(who, s, result, error, routine, &args, &sql);
  args.clear();
  m_args = std::move(args);
  return ok && routine.store_results(result);
}

void Session::forget(const Routine &routine) {
  m_programs.erase(routine.id);
  for (auto &[principal, slot] : m_workers) {
    if (slot.prepared.erase(routine.id) == 0) continue;
    std::string error;
    if (slot.worker && slot.worker->usable())
      (void)slot.worker->drop(routine.id, &error);
  }
}

void Session::keep_console(const ResultMsg &result) {
  if (result.console_cleared) {
    m_out.clear();
    m_err.clear();
  }
  append_capped(&m_out, result.console_out);
  append_capped(&m_err, result.console_err);
}

void Session::report(const std::string &principal, Status status,
                     const ResultMsg &result, const std::string &error,
                     const Routine &routine, const std::vector<Value> *args,
                     SqlRunner *sql) {
  const mle::Error *e = result.error ? &*result.error : nullptr;
  // A MySQL error from the routine (S-3, S-11): an inner statement's error
  // still in the diagnostics area reaches the client unchanged; an
  // SqlError the routine threw is raised like SIGNAL.
  if (status == Status::kGuestError && e != nullptr && e->sql_errno != 0 &&
      sql != nullptr) {
    if (sql->propagates(*e) || sql->raise(*e)) return;
  }
  if (sql != nullptr) sql->clear();
  switch (status) {
    case Status::kGuestError:
    case Status::kExhausted: {
      if (status == Status::kExhausted) ++g_counters.oom_errors;
      if (e == nullptr) break;
      if (e->language == "MLE-Type") {
        std::string message = "MLE-Type> " + e->message;
        if (args != nullptr)
          message += " in '" + routine.call_text(*args) + "'";
        my_error(ER_LANGUAGE_COMPONENT, MYF(0), message.c_str());
        return;
      }
      const char *language = e->language.empty()
                                 ? routine.display_language()
                                 : display_language(e->language);
      keep_stack_trace(*e, routine);
      my_error(ER_LANGUAGE_COMPONENT_RUNTIME, MYF(0), language,
               e->message.c_str());
      const auto it = m_workers.find(principal);
      if (it != m_workers.end() &&
          (!it->second.worker || !it->second.worker->usable())) {
        // Retired after running out of heap or stack.
        log_warning("The worker of " + principal +
                    " was retired: " + e->message);
        drop_worker(principal);
      }
      return;
    }
    case Status::kInterrupted:
      // A KILL or max_execution_time: the SQL layer reports it.
      if (Hooks(m_thd).killed()) return;
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      return;
    default:
      break;
  }
  // The worker is lost (died, killed, misbehaved) or failed internally.
  const std::string why =
      !error.empty() ? error : e != nullptr ? e->message : "no details";
  if (status != Status::kInternal) {
    ++g_counters.worker_failures;
    if (why.find("out-of-memory") != std::string::npos) ++g_counters.oom_errors;
    log_warning("The worker of " + principal + " was lost during " +
                routine.qualified_name + ": " + why);
    drop_worker(principal);
    if (status == Status::kKilled && Hooks(m_thd).killed()) return;
  } else {
    log_error("Internal error in the worker of " + principal + " during " +
              routine.qualified_name + ": " + why);
  }
  my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
}

}  // namespace mle_component

namespace mle_component {

// ------------------------------------------------------------- state

void Session::keep_stack_trace(const mle::Error &e, const Routine &routine) {
  // Oracle: "<js> pc2:3:6-54", one line per frame.
  const std::string language =
      e.language.empty() ? routine.prepare.language : e.language;
  m_stack.clear();
  for (const mle::StackFrame &f : e.stack) {
    m_stack += "<" + language + "> " + f.file + ":" + std::to_string(f.line);
    if (f.column > 0) {
      m_stack += ":" + std::to_string(f.column);
      if (f.end_column > f.column) m_stack += "-" + std::to_string(f.end_column);
    }
    m_stack += "\n";
  }
}

std::optional<std::string> Session::state(const Session *s,
                                          const std::string &key) {
  auto count = [&](uint16_t type) {
    long n = 0;
    if (s != nullptr)
      for (const auto &[id, t] : s->m_programs) n += (type == 0 || t == type);
    return std::to_string(n);
  };
  if (key == "is_active")
    return s != nullptr && !s->m_workers.empty() ? "1" : "0";
  if (key == "stdout") return s != nullptr ? s->m_out : "";
  if (key == "stderr") return s != nullptr ? s->m_err : "";
  if (key == "stack_trace") return s != nullptr ? s->m_stack : "";
  if (key == "stored_functions")
    return count(MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION);
  if (key == "stored_procedures")
    return count(MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE);
  if (key == "stored_programs") return count(0);
  return std::nullopt;
}

long Session::worker_pid() {
  const auto it = m_workers.find(principal());
  if (it == m_workers.end() || !it->second.worker) return -1;
  return it->second.worker->process().pid();
}

void Session::reset_output(bool out, bool err) {
  if (out) m_out.clear();
  if (err) m_err.clear();
}

void Session::reset() {
  for (auto &[principal, slot] : m_workers)
    engine_release_worker(std::move(slot.worker));
  m_workers.clear();
  m_programs.clear();
  m_stack.clear();
  m_out.clear();
  m_err.clear();
  m_options = mle::OptionsMsg();
  m_options_bound = false;
  ++g_counters.session_resets;
}

bool Session::set_options(mle::wire::IntegerType integer_type,
                          mle::wire::DecimalType decimal_type) {
  if (has_programs()) return false;
  m_options.integer_type = integer_type;
  m_options.decimal_type = decimal_type;
  for (auto &[principal, slot] : m_workers) {
    std::string error;
    if (slot.worker && slot.worker->usable() &&
        slot.worker->options(m_options, &error) != Status::kOk)
      log_warning("Options for the worker of " + principal + ": " + error);
  }
  return true;
}

// ------------------------------------------------------------- privileges

namespace {
constexpr const char *kPrivileges[] = {"CREATE_JAVASCRIPT_ROUTINE",
                                       "CREATE_PYTHON_ROUTINE",
                                       "CREATE_JAVA_ROUTINE"};

const char *privilege_of(const Routine &r) {
  return r.language == kJavaScript ? kPrivileges[0]
         : r.language == kPython   ? kPrivileges[1]
                                   : kPrivileges[2];
}
}  // namespace

bool register_privileges() {
  for (const char *p : kPrivileges)
    if (mysql_service_dynamic_privilege_register->register_privilege(
            p, strlen(p)))
      return true;
  return false;
}

void unregister_privileges() {
  for (const char *p : kPrivileges)
    (void)mysql_service_dynamic_privilege_register->unregister_privilege(
        p, strlen(p));
}

bool check_create_privilege(MYSQL_THD thd, const Routine &routine) {
  mysql_cstring_with_length command{nullptr, 0};
  if (mysql_service_mysql_thd_attributes->get(thd, "sql_command", &command) ||
      command.str == nullptr)
    return true;
  // sql_statement_names (mysqld.cc): SQLCOM_CREATE_SPFUNCTION is
  // "create_function" (a UDF is "create_udf").
  const std::string cmd(command.str, command.length);
  if (cmd != "create_procedure" && cmd != "create_function") return true;
  Security_context_handle sctx = nullptr;
  if (mysql_service_mysql_thd_security_context->get(thd, &sctx)) return true;
  const char *privilege = privilege_of(routine);
  if (mysql_service_global_grants_check->has_global_grant(sctx, privilege,
                                                          strlen(privilege)))
    return true;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege);
  return false;
}

}  // namespace mle_component
