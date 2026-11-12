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
#include <mutex>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/mysql_thd_attributes_bits.h>
#include <mysql/components/services/mysql_library.h>
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
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_library);
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

/** Read by every session thread, set at INSTALL and UNINSTALL. */
std::atomic<mysql_thd_store_slot> g_slot{nullptr};
std::atomic<long> g_sessions{0};

constexpr size_t kConsoleKeep = 64 * 1024;

/** The SQL layer's side of a call: SQL from routines (Phase 5) and KILL. */
/** Forgets an error the server put in the diagnostics area meanwhile. */
void clear_server_error(MYSQL_THD thd) {
  uint16_t empty = STATUS_DA_EMPTY;
  (void)mysql_service_mysql_thd_attributes->set(thd, "da_status", &empty);
}

class Hooks : public mle::CallHooks {
 public:
  explicit Hooks(MYSQL_THD thd, SqlRunner *sql = nullptr,
                 const Routine *routine = nullptr)
      : m_thd(thd), m_sql(sql), m_routine(routine) {}

  /**
    A database library the routine imports (Phase 6, 14 §2). Importable:
    the routine's schema, its USING libraries and their schemas (the server
    checked EXECUTE on the USING libraries at CREATE; components cannot check
    routine privileges at run time).
  */
  void library(const mle::LibraryMsg &m, mle::LibraryResultMsg *r) override {
    r->status = mle::wire::LibraryStatus::kRefused;
    const std::string what = m.schema + "." + m.name;
    if (m_routine == nullptr) {
      r->message = "library " + what + ": no routine to import it";
      return;
    }
    bool allowed = m.schema == m_routine->schema;
    for (const mle::Import &i : m_routine->prepare.imports)
      allowed = allowed || i.schema == m.schema;
    if (!allowed) {
      r->message = "library " + what + " is not importable from " +
                   m_routine->qualified_name +
                   " (import it with USING, or use a library of the routine's schema)";
      return;
    }
    my_h_library h = nullptr;
    if (mysql_service_mysql_library->init(
            m_thd, {m.schema.data(), m.schema.size()}, {m.name.data(), m.name.size()},
            {"", 0}, &h) ||
        h == nullptr) {
      r->message = "library " + what + " does not exist";
      clear_server_error(m_thd);
      return;
    }
    mysql_cstring_with_length body{nullptr, 0}, language{nullptr, 0};
    if (mysql_service_mysql_library->get_body(h, &body) ||
        mysql_service_mysql_library->get_language(h, &language)) {
      (void)mysql_service_mysql_library->deinit(h);
      r->message = "library " + what + " cannot be read";
      return;
    }
    const std::string_view text(body.str == nullptr ? "" : body.str, body.length);
    r->hash = mle::library_hash(text);
    const unsigned bit = language_bit({language.str, language.length});
    r->language = bit == kJavaScript ? "js" : bit == kPython ? "python" : bit == kJava ? "java" : "";
    if (r->language.empty()) {
      r->message = "library " + what + " is in a language MLE does not run";
    } else if (r->hash == m.known_hash) {
      r->status = mle::wire::LibraryStatus::kUnchanged;
    } else {
      r->status = mle::wire::LibraryStatus::kBody;
      r->body.assign(text);
    }
    (void)mysql_service_mysql_library->deinit(h);
  }

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
  const Routine *m_routine;
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

/** Text from a worker or routine for the error log: one line, bounded (a
    routine must not forge log lines or fill the log). */
std::string log_safe(const std::string &text) {
  std::string out;
  for (unsigned char c : text.substr(0, 1000))
    out += c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
  if (text.size() > 1000) out += "...";
  return out;
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
    ++s->m_calls;
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
    if (--m_session->m_calls == 0) {
      // The outermost request ended: no request runs in a retired worker.
      for (auto &w : m_session->m_retired) engine_release_worker(std::move(w));
      m_session->m_retired.clear();
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
  mysql_thd_store_slot slot = nullptr;
  const bool failed =
      mysql_service_mysql_thd_store->register_slot("mle", free_session, &slot);
  if (!failed) g_slot = slot;
  return failed;
}

void Session::unregister_slot() {
  mysql_thd_store_slot slot = g_slot.exchange(nullptr);
  if (slot != nullptr) (void)mysql_service_mysql_thd_store->unregister_slot(slot);
}

long Session::count() { return g_sessions.load(); }

Session *Session::of(MYSQL_THD thd) {
  mysql_thd_store_slot slot = g_slot.load();
  if (slot == nullptr || thd == nullptr) return nullptr;
  return static_cast<Session *>(mysql_service_mysql_thd_store->get(thd, slot));
}

Session *Session::current(bool create) {
  MYSQL_THD thd = nullptr;
  if (mysql_service_mysql_current_thread_reader->get(&thd) || thd == nullptr)
    return nullptr;
  Session *s = of(thd);
  if (s != nullptr || !create) return s;
  s = new Session(thd);
  if (mysql_service_mysql_thd_store->set(thd, g_slot.load(), s)) {
    delete s;
    return nullptr;
  }
  raise_max(g_counters.sessions_max, ++g_sessions);
  g_counters.sessions = g_sessions.load();
  return s;
}

Session::~Session() {
  for (auto &[principal, slot] : m_workers)
    engine_release_worker(std::move(slot.worker), principal);
  for (auto &w : m_retired) engine_release_worker(std::move(w));
  g_counters.sessions = --g_sessions;
}

std::string account_key(std::string_view user, std::string_view host) {
  // MySQL's quoting: unambiguous ('a@b'@'c' is not 'a'@'b@c').
  std::string key = "'";
  for (char c : user) key += c == '\'' ? std::string("''") : std::string(1, c);
  key += "'@'";
  for (char c : host) key += c == '\'' ? std::string("''") : std::string(1, c);
  key += '\'';
  return key;
}

std::string Session::principal() const {
  // Read on every call: a security context's address says nothing (a
  // COM_CHANGE_USER rewrites the session's context in place, a new routine
  // may get a dropped one's address).
  Security_context_handle sctx = nullptr;
  MYSQL_LEX_CSTRING user{"", 0}, host{"", 0};
  if (mysql_service_mysql_thd_security_context->get(m_thd, &sctx) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_user",
                                                        &user) ||
      mysql_service_mysql_security_context_options->get(sctx, "priv_host",
                                                        &host))
    return "?";
  return account_key({user.str, user.length}, {host.str, host.length});
}

// ------------------------------------------------------------- workers

void Session::drop_worker(const std::string &principal) {
  auto it = m_workers.find(principal);
  if (it == m_workers.end()) return;
  // Outer requests of this session may still run in it (nested calls).
  if (m_calls > 0)
    m_retired.push_back(std::move(it->second.worker));
  else
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
  // A worker kept from an earlier session of this principal (warm), else a
  // new one from the pool. A kept worker that died meanwhile fails its
  // OPTIONS: then a new one.
  std::unique_ptr<mle::Worker> worker =
      engine_reuse_worker(principal, max_workers());
  if (worker && worker->options(m_options, &error) != Status::kOk) {
    engine_release_worker(std::move(worker));
    error.clear();
  }
  const bool reused = worker != nullptr;
  Hooks hooks(m_thd);
  if (!worker)
    worker = engine_new_worker(max_workers(), spin_us(), interrupt_grace_ms(),
                               &hooks, &why, &error);
  if (why == AcquireError::kFailed && hooks.killed())
    return nullptr;  // KILL while waiting for the worker: the server reports it
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
      log_error("No worker for " + principal + ": " + log_safe(error));
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return nullptr;
  }

  if (!reused && worker->options(m_options, &error) != Status::kOk) {
    engine_release_worker(std::move(worker));
    if (error.find("unknown time zone") != std::string::npos) {
      my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0),
               routine.display_language(), zone.c_str());
    } else {
      log_error("A new worker for " + principal + " failed: " + log_safe(error));
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
  Hooks hooks(m_thd, nullptr, &routine);
  ResultMsg result;
  std::string error;
  Status s;
  mle::Worker *worker = slot.worker.get();
  {
    const KillGuard kill(this, worker);
    s = worker->prepare(routine.prepare, hooks, &result, &error);
  }
  keep_console(result);
  if (s != Status::kOk) {
    report(principal, s, result, error, routine, nullptr);
    return false;
  }
  // Loading runs routine code, which may run SQL and nested calls; one of
  // them may have retired this worker (and its slot) meanwhile.
  const auto it = m_workers.find(principal);
  if (it == m_workers.end() || it->second.worker.get() != worker) {
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    return false;
  }
  it->second.prepared.insert(routine.id);
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
  const std::string who = principal();  // a copy: nested calls change it
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr || !prepare_in(who, *slot, routine)) return false;

  SqlRunner sql(m_thd);  // the call's statements; closed when it returns
  Hooks hooks(m_thd, &sql, &routine);
  ResultMsg result;
  std::string error;
  raise_max(g_counters.threads_max, ++g_counters.threads);
  Status s;
  const uint64_t kills = m_kills.load();
  slot->worker->set_timing(spin_us(), interrupt_grace_ms());
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
                    " was retired: " + log_safe(e->message));
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
                log_safe(routine.qualified_name) + ": " + log_safe(why));
    drop_worker(principal);
    if (status == Status::kKilled && Hooks(m_thd).killed()) return;
    // The caller learns why (the worker died: signal ..., did not stop after
    // KILL ...); the next call of the session gets a new worker.
    my_error(ER_LANGUAGE_COMPONENT_RUNTIME, MYF(0), "MLE", why.c_str());
    return;
  }
  log_error("Internal error in the worker of " + principal + " during " +
            log_safe(routine.qualified_name) + ": " + log_safe(why));
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

void Session::clear_state(bool reuse) {
  for (auto &[principal, slot] : m_workers)
    engine_release_worker(std::move(slot.worker), reuse ? principal : "");
  m_workers.clear();
  m_programs.clear();
  m_stack.clear();
  m_out.clear();
  m_err.clear();
  m_options = mle::OptionsMsg();
  m_options_bound = false;
}

bool Session::reset() {
  // A routine of this session runs (it called mle_session_reset() through
  // SQL): its worker must not go away under it.
  if (m_calls > 0) return false;
  // Asked for a clean state: new workers, not this account's kept ones.
  clear_state(false);
  ++g_counters.session_resets;
  return true;
}

void Session::account_changed() {
  if (m_calls > 0) return;  // between commands: never (defensive)
  clear_state(true);
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

// ------------------------------------------------------------- server paths

std::vector<std::string> server_private_paths() {
  // Read once (at INSTALL): they do not change while mysqld runs, and later
  // engine restarts come from SET GLOBAL, which holds the system variables'
  // lock the reader takes.
  static std::mutex lock;
  static std::vector<std::string> known;
  static bool done = false;
  std::lock_guard<std::mutex> guard(lock);
  if (done) return known;
  std::vector<std::string> out;
  MYSQL_THD thd = nullptr;
  // GLOBAL values are read through the current THD (none: skip).
  if (mysql_service_mysql_current_thread_reader->get(&thd) || thd == nullptr)
    return out;
  for (const char *name : {"plugin_dir", "datadir"}) {
    char buffer[PATH_MAX];
    void *value = buffer;
    size_t length = sizeof buffer - 1;
    if (!mysql_service_mysql_system_variable_reader->get(
            thd, "GLOBAL", "mysql_server", name, &value, &length) &&
        length > 0)
      out.emplace_back(static_cast<const char *>(value), length);
  }
  known = out;
  done = true;
  return out;
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

bool Session::parse_library(unsigned language, const std::string &name,
                            const std::string &body) {
  Routine routine;  // for the worker and the messages
  routine.language = language;
  routine.name = name;
  routine.qualified_name = name;
  // A library is code for the language's routines: the same privilege
  // (D-3) as CREATE FUNCTION / PROCEDURE in that language.
  if (!has_global_privilege(m_thd, privilege_of(routine))) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege_of(routine));
    return false;
  }
  const std::string who = principal();
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr) return false;
  mle::ParseLibraryMsg m;
  m.language = language == kJavaScript ? "js" : language == kPython ? "python" : "java";
  m.name = name;
  m.body = body;
  Hooks hooks(m_thd);
  ResultMsg result;
  std::string error;
  Status s;
  {
    const KillGuard kill(this, slot->worker.get());
    s = slot->worker->parse_library(m, hooks, &result, &error);
  }
  if (s == Status::kOk) return true;
  report(who, s, result, error, routine, nullptr);
  return false;
}

bool has_global_privilege(MYSQL_THD thd, const char *privilege) {
  Security_context_handle sctx = nullptr;
  if (mysql_service_mysql_thd_security_context->get(thd, &sctx)) return false;
  return mysql_service_global_grants_check->has_global_grant(
      sctx, privilege, strlen(privilege));
}

bool check_create_privilege(MYSQL_THD thd, const Routine &routine) {
  mysql_cstring_with_length command{nullptr, 0};
  if (mysql_service_mysql_thd_attributes->get(thd, "sql_command", &command) ||
      command.str == nullptr) {
    // Cannot tell whether this is a CREATE: refuse (fail closed).
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    return false;
  }
  // sql_statement_names (mysqld.cc): SQLCOM_CREATE_SPFUNCTION is
  // "create_function" (a UDF is "create_udf").
  const std::string cmd(command.str, command.length);
  if (cmd != "create_procedure" && cmd != "create_function") return true;
  Security_context_handle sctx = nullptr;
  if (mysql_service_mysql_thd_security_context->get(thd, &sctx)) {
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    return false;
  }
  const char *privilege = privilege_of(routine);
  if (mysql_service_global_grants_check->has_global_grant(sctx, privilege,
                                                          strlen(privilege)))
    return true;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege);
  return false;
}

}  // namespace mle_component
