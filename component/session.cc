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
#include <cstdlib>
#include <ctime>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/mysql_thd_attributes_bits.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/components/services/mysql_thd_attributes.h>
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
  explicit Hooks(MYSQL_THD thd) : m_thd(thd) {}

  void sql(const mle::SqlMsg &, mle::SqlResultMsg *r) override {
    r->kind = mle::SqlResultKind::kError;
    r->sql_errno = ER_NOT_SUPPORTED_YET;
    r->sqlstate = "42000";
    r->message =
        "This version of MySQL doesn't yet support 'SQL statements in MLE "
        "routines'";
  }

  /** KILL QUERY / KILL CONNECTION / max_execution_time. */
  bool killed() override {
    uint16_t status = STATUS_SESSION_OK;
    mysql_service_mysql_thd_attributes->get(m_thd, "thd_status", &status);
    return status != STATUS_SESSION_OK;
  }

 private:
  MYSQL_THD m_thd;
};

/** The zone of the system (time_zone = SYSTEM): an IANA name if known,
    else the current UTC offset. */
std::string system_time_zone() {
  char target[PATH_MAX];
  const ssize_t n = readlink("/etc/localtime", target, sizeof target - 1);
  if (n > 0) {
    const std::string path(target, static_cast<size_t>(n));
    const size_t at = path.find("zoneinfo/");
    if (at != std::string::npos) return path.substr(at + 9);
  }
  if (const char *tz = getenv("TZ"); tz != nullptr && *tz != '\0')
    return tz[0] == ':' ? tz + 1 : tz;
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  const long off = local.tm_gmtoff;
  char b[48];
  snprintf(b, sizeof b, "%c%02ld:%02ld", off < 0 ? '-' : '+', labs(off) / 3600,
           labs(off) % 3600 / 60);
  return b;
}

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

std::string Session::principal() const {
  Security_context_handle sctx = nullptr;
  MYSQL_LEX_CSTRING user{"", 0}, host{"", 0};
  if (mysql_service_mysql_thd_security_context->get(m_thd, &sctx) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_user",
                                                        &user) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_host",
                                                        &host))
    return "?";
  return std::string(user.str, user.length) + "@" +
         std::string(host.str, host.length);
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
  const Status s = slot.worker->prepare(routine.prepare, hooks, &result, &error);
  keep_console(result);
  if (s != Status::kOk) {
    report(principal, s, result, error, routine, nullptr);
    return false;
  }
  slot.prepared.insert(routine.id);
  return true;
}

// ------------------------------------------------------------- requests

bool Session::parse(Routine &routine) {
  const std::string who = principal();
  Slot *slot = worker_for(who, routine);
  return slot != nullptr && prepare_in(who, *slot, routine);
}

bool Session::execute(Routine &routine) {
  std::vector<Value> args;
  if (!routine.read_args(&args)) return false;
  const std::string who = principal();
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr || !prepare_in(who, *slot, routine)) return false;

  Hooks hooks(m_thd);
  ResultMsg result;
  std::string error;
  raise_max(g_counters.threads_max, ++g_counters.threads);
  const Status s =
      slot->worker->call(routine.id, 0, args, hooks, &result, &error);
  --g_counters.threads;
  keep_console(result);
  if (s != Status::kOk) {
    report(who, s, result, error, routine, &args);
    return false;
  }
  return routine.store_results(result);
}

void Session::forget(const Routine &routine) {
  for (auto &[principal, slot] : m_workers) {
    if (slot.prepared.erase(routine.id) == 0) continue;
    std::string error;
    if (slot.worker && slot.worker->usable())
      (void)slot.worker->drop(routine.id, &error);
  }
}

void Session::keep_console(const ResultMsg &result) {
  append_capped(&m_out, result.console_out);
  append_capped(&m_err, result.console_err);
}

void Session::report(const std::string &principal, Status status,
                     const ResultMsg &result, const std::string &error,
                     const Routine &routine, const std::vector<Value> *args) {
  const mle::Error *e = result.error ? &*result.error : nullptr;
  switch (status) {
    case Status::kGuestError:
    case Status::kExhausted: {
      if (status == Status::kExhausted) ++g_counters.oom_errors;
      if (e == nullptr) break;
      if (e->sql_errno != 0) {
        // A MySQL error that crossed the routine (Phase 5 SQL API).
        my_error(ER_LANGUAGE_COMPONENT, MYF(0), e->message.c_str());
        return;
      }
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
