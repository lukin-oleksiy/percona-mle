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

#include "my_sqlcommand.h"  // enum_sql_command (CREATE checks)

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
#include <mysql/components/services/mysql_runtime_warning.h>
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
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_warning);
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
                 const Routine *routine = nullptr,
                 const char *no_sql = "SQL is only possible while a routine runs")
      : m_thd(thd), m_sql(sql), m_routine(routine), m_no_sql(no_sql) {}

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
    // A failed lookup may leave the server's error (a missing library, an
    // MDL timeout, KILL) in the DA: it is not the routine's, the routine
    // gets r->message. An inner statement's error kept there goes with it,
    // and the SqlRunner must know (or a rethrown SQL error would be taken as
    // still raised).
    const auto lookup_failed = [&](std::string message) {
      r->message = std::move(message);
      clear_server_error(m_thd);
      if (m_sql != nullptr) m_sql->clear();
    };
    my_h_library h = nullptr;
    if (mysql_service_mysql_library->init(
            m_thd, {m.schema.data(), m.schema.size()}, {m.name.data(), m.name.size()},
            {"", 0}, &h) ||
        h == nullptr) {
      lookup_failed("library " + what + " does not exist");
      return;
    }
    mysql_cstring_with_length body{nullptr, 0}, language{nullptr, 0};
    if (mysql_service_mysql_library->get_body(h, &body) ||
        mysql_service_mysql_library->get_language(h, &language)) {
      (void)mysql_service_mysql_library->deinit(h);
      // The server's init() succeeds for a library that does not exist;
      // reading its body is what fails.
      lookup_failed("library " + what + " does not exist or cannot be read");
      return;
    }
    const std::string_view text(body.str == nullptr ? "" : body.str, body.length);
    r->hash = mle::library_hash(text);
    const unsigned bit = language_bit({language.str, language.length});
    r->language = worker_language(bit);
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
    sql_into(m, r, nullptr);
  }
  /** The same; rows may go straight into the mailbox arena (exp note 44). */
  void sql_into(const mle::SqlMsg &m, mle::SqlResultMsg *r,
                mle::DirectReply *direct) override {
    if (m_sql != nullptr) return m_sql->handle(m, r, direct);
    sql_error(r, ER_LANGUAGE_COMPONENT_INTERNAL, m_no_sql);
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
  const char *m_no_sql;
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
  if (buffer->size() > kConsoleKeep) {
    // The tail, from a whole UTF-8 character.
    size_t from = buffer->size() - kConsoleKeep;
    while (from < buffer->size() &&
           (static_cast<unsigned char>((*buffer)[from]) & 0xC0) == 0x80)
      from++;
    buffer->erase(0, from);
  }
}


/**
  A failure of the engine rather than of the routine's code (a lost worker,
  the engine not running): ER_LANGUAGE_COMPONENT (6000) with "MLE> " and the
  reason. Exceptions thrown by routine code are ER_LANGUAGE_COMPONENT_RUNTIME
  (6113, "<language>> message"), so handlers and retry logic can tell them
  apart; internal errors are ER_LANGUAGE_COMPONENT_INTERNAL (6111), with the
  details in the error log only.
*/
/** A frame of the routine's body itself, not of a function in it. */
bool body_frame(const mle::StackFrame &f) {
  // ":=>": the wrapper function around a JavaScript body.
  return f.function.empty() || f.function.rfind("__mle", 0) == 0 ||
         f.function.rfind(":", 0) == 0 || f.function == "<module>" ||
         f.function == f.file;
}

/** " (routine, line N, in function)" at the innermost frame; "" without one. */
std::string error_location(const mle::Error &e) {
  if (e.stack.empty() || e.stack.front().file.empty()) return "";
  const mle::StackFrame &f = e.stack.front();
  std::string s = " (" + f.file;
  if (f.line > 0) s += ", line " + std::to_string(f.line);
  if (!body_frame(f)) s += ", in " + f.function;
  return s + ")";
}

/**
  The error as its language shows it, with the frames' source lines: Python's
  "Traceback (most recent call last)", "at function (routine:line:column)"
  for JavaScript and Java.
*/
std::string traceback_text(const mle::Error &e, const std::string &language,
                           const std::string &routine, bool routine_text,
                           bool other_text) {
  // The source line of a frame: of the routine's code or of other code (a
  // library's), for a reader who may see that code.
  const auto shown = [&](const mle::StackFrame &f) {
    return !f.text.empty() && (f.file == routine ? routine_text : other_text);
  };
  std::string t;
  if (language == "python") {
    t = "Traceback (most recent call last):\n";
    for (auto it = e.stack.rbegin(); it != e.stack.rend(); ++it) {
      t += "  File \"" + it->file + "\", line " + std::to_string(it->line) + ", in " +
           (body_frame(*it) ? std::string("<routine>") : it->function) + "\n";
      if (shown(*it)) t += "    " + it->text + "\n";
    }
    return t + e.message + "\n";
  }
  t = e.message + "\n";
  for (const mle::StackFrame &f : e.stack) {
    std::string where = f.file + ":" + std::to_string(f.line);
    if (f.column > 0) where += ":" + std::to_string(f.column);
    t += "    at " + (body_frame(f) ? where : f.function + " (" + where + ")") + "\n";
    if (shown(f)) t += "        " + f.text + "\n";
  }
  return t;
}

void engine_error(std::string why) {
  // The host's reasons name "the MLE worker"; after "MLE> " once is enough.
  constexpr std::string_view kWorker = "the MLE worker";
  if (why.compare(0, kWorker.size(), kWorker) == 0)
    why.replace(0, kWorker.size(), "the worker");
  my_error(ER_LANGUAGE_COMPONENT, MYF(0), ("MLE> " + why).c_str());
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

long Session::count() { return g_counters.sessions.load(); }

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
  return s;
}

// Counted where it is made and where it goes (one atomic counter, which the
// status variable shows): a Session deleted before it was stored counts too.
Session::Session(MYSQL_THD thd) : m_thd(thd), m_mark(engine_session_begin()) {
  raise_max(g_counters.sessions_max, ++g_counters.sessions);
}

Session::~Session() {
  for (auto &[key, slot] : m_workers)
    engine_release_worker(std::move(slot.worker), slot.kind, slot.principal, m_mark);
  for (auto &w : m_retired) engine_release_worker(std::move(w));
  engine_session_end(m_mark);
  --g_counters.sessions;
}

std::string account_key(std::string_view user, std::string_view host) {
  // MySQL's quoting: unambiguous ('a@b'@'c' is not 'a'@'b@c').
  // Built in place: it runs on every call (exp note 37).
  std::string key;
  key.reserve(user.size() + host.size() + 8);
  key += '\'';
  for (char c : user) {
    key += c;
    if (c == '\'') key += c;
  }
  key += "'@'";
  // Host names compare without case (a definer's host may be written in
  // another case than the account's).
  for (char c : host) {
    key += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (c == '\'') key += c;
  }
  key += '\'';
  return key;
}

bool principal_has_account(std::string_view principal,
                           std::string_view account) {
  if (principal == account) return true;
  const size_t n = account.size() + kCodeOf.size();
  return principal.size() > n &&
         ((principal.starts_with(account) &&
           principal.substr(account.size(), kCodeOf.size()) == kCodeOf) ||
          (principal.ends_with(account) &&
           principal.substr(principal.size() - n, kCodeOf.size()) == kCodeOf));
}

std::string Session::principal(const Routine *routine) const {
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
  std::string key =
      account_key({user.str, user.length}, {host.str, host.length});
  // A DEFINER routine runs as its definer: the keys are equal.
  if (routine != nullptr && !routine->definer_key.empty() &&
      routine->definer_key != key) {
    key += kCodeOf;
    key += routine->definer_key;
  }
  return key;
}

// ------------------------------------------------------------- workers

std::string Session::slot_key(const std::string &principal, unsigned language) {
  std::string key = principal;
  key += '\0';
  key += kind_name(kind_of(language));
  return key;
}

std::string Session::display_key(const std::string &key) {
  const size_t nul = key.find('\0');
  if (nul == std::string::npos) return key;
  return key.substr(0, nul) + " (" + key.substr(nul + 1) + ")";
}

void Session::drop_worker(const std::string &key) {
  auto it = m_workers.find(key);
  if (it == m_workers.end()) return;
  // Outer requests of this session may still run in it (nested calls).
  if (m_calls > 0)
    m_retired.push_back(std::move(it->second.worker));
  else
    engine_release_worker(std::move(it->second.worker), it->second.kind);
  m_workers.erase(it);
}

Session::Slot *Session::worker_for(const std::string &key,
                                   const Routine &routine) {
  auto it = m_workers.find(key);
  if (it != m_workers.end()) {
    if (it->second.worker && it->second.worker->usable()) return &it->second;
    drop_worker(key);  // died or retired: a fresh one below
  }
  const std::string principal = key.substr(0, key.find('\0'));
  const WorkerKind kind = kind_of(routine.language);

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
      engine_reuse_worker(principal, kind, max_workers());
  if (worker && worker->options(m_options, &error) != Status::kOk) {
    engine_release_worker(std::move(worker), kind);
    error.clear();
  }
  const bool reused = worker != nullptr;
  Hooks hooks(m_thd);
  if (!worker)
    worker = engine_new_worker(kind, max_workers(), spin_us(), interrupt_grace_ms(),
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
      engine_error("The MLE engine is not running: " + error);
      return nullptr;
    case AcquireError::kFailed:
      log_error("No worker for " + display_key(key) + ": " + log_safe(error));
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return nullptr;
  }

  if (!reused && worker->options(m_options, &error) != Status::kOk) {
    engine_release_worker(std::move(worker), kind);
    if (error.find("unknown time zone") != std::string::npos) {
      my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0),
               routine.display_language(), zone.c_str());
    } else {
      log_error("A new worker for " + display_key(key) + " failed: " + log_safe(error));
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    }
    return nullptr;
  }
  Slot &slot = m_workers[key];
  slot.worker = std::move(worker);
  slot.principal = principal;
  slot.kind = kind;
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
    m_console_now = console_warnings(m_thd);  // this failure's, not the last call's
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
  const std::string who = slot_key(principal(&routine), routine.language);
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr) return false;
  // CREATE: the worker also checks the code (exp note 39); its findings
  // become the statement's warnings.
  enum_sql_command command = SQLCOM_END;
  (void)mysql_service_mysql_thd_attributes->get(m_thd, "sql_command_id", &command);
  routine.prepare.check =
      command == SQLCOM_CREATE_PROCEDURE || command == SQLCOM_CREATE_SPFUNCTION;
  const bool ok = prepare_in(who, *slot, routine);
  const bool checked = routine.prepare.check;
  routine.prepare.check = false;
  if (ok && checked) check_findings_to_warnings(routine);
  return ok;
}

void Session::check_findings_to_warnings(const Routine &routine) {
  // "\x1f" CHECK_MARK "message" lines of the PREPARE's stderr: warnings, and
  // not the session's output.
  constexpr std::string_view kMark = "\x1fmle-check:";
  auto take = [&](std::string *text, bool emit) {
    std::string kept;
    size_t start = 0;
    while (start < text->size()) {
      size_t end = text->find('\n', start);
      if (end == std::string::npos) end = text->size() - 1;
      const std::string_view line(text->data() + start, end - start + 1);
      if (line.substr(0, kMark.size()) == kMark) {
        if (emit) {
          std::string message = std::string(routine.display_language()) + "> " + routine.name +
                                ", " + std::string(line.substr(kMark.size()));
          while (!message.empty() && message.back() == '\n') message.pop_back();
          mysql_service_mysql_runtime_warning->emit(m_thd, ER_LANGUAGE_COMPONENT,
                                                    message.c_str());
        }
      } else {
        kept.append(line);
      }
      start = end + 1;
    }
    *text = std::move(kept);
  };
  take(&m_last_err, true);
  take(&m_err, false);
}

bool Session::execute(Routine &routine) {
  m_stack.clear();  // Oracle: the trace of the last call only
  m_trace.reset();
  std::vector<Value> args = std::move(m_args);  // reused; a nested call allocates
  if (!routine.read_args(&args)) return false;
  const std::string who = slot_key(principal(&routine), routine.language);  // a copy: nested calls change it
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr || !prepare_in(who, *slot, routine)) return false;

  // Whose code runs, for what it evaluates with mle_eval(): restored when
  // the call returns (a nested call has its own).
  struct Running {
    const Routine *&slot;
    const Routine *saved;
    ~Running() { slot = saved; }
  } running{m_running_routine, m_running_routine};
  m_running_routine = &routine;

  // The call's statements; closed when it returns.
  SqlRunner sql(m_thd, &m_results_held);
  Hooks hooks(m_thd, &sql, &routine);
  ResultMsg result;
  std::string error;
  Status s;
  const uint64_t kills = m_kills.load();
  slot->worker->set_timing(spin_us(), interrupt_grace_ms());
  {
    // mle_threads: counted while the call runs, released if it throws too.
    struct Running {
      Running() { raise_max(g_counters.threads_max, ++g_counters.threads); }
      ~Running() { --g_counters.threads; }
    } running;
    const KillGuard kill(this, slot->worker.get());
    s = slot->worker->call(routine.id, 0, args, hooks, &result, &error);
  }
  keep_console(result);
  // mle.console = WARNINGS: read only when there is something to show.
  m_console_now = (s != Status::kOk || !result.console_out.empty() ||
                   !result.console_err.empty()) &&
                  console_warnings(m_thd);
  if (m_console_now) console_to_warnings(routine, result);
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

namespace {
constexpr size_t kMaxConsoleWarnings = 100;  // per call; the rest: last_output
}  // namespace

void Session::console_to_warnings(const Routine &routine, const ResultMsg &result) {
  lines_to_warnings(result.console_out, routine.name);
  lines_to_warnings(result.console_err, routine.name + " stderr");
}

void Session::lines_to_warnings(const std::string &text, const std::string &prefix) {
  size_t n = 0, start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (n < kMaxConsoleWarnings) {
      std::string line = prefix + ": " + text.substr(start, end - start);
      if (line.size() > 500) {
        size_t cut = 497;  // at a whole UTF-8 character
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
          cut--;
        line = line.substr(0, cut) + "...";
      }
      mysql_service_mysql_runtime_warning->emit(m_thd, ER_LANGUAGE_COMPONENT, line.c_str());
    }
    ++n;
    start = end + 1;
  }
  if (n > kMaxConsoleWarnings) {
    const std::string more = prefix + ": ... " + std::to_string(n - kMaxConsoleWarnings) +
                             " more lines: mle_session_state('last_output')";
    mysql_service_mysql_runtime_warning->emit(m_thd, ER_LANGUAGE_COMPONENT, more.c_str());
  }
}

void Session::keep_console(const ResultMsg &result) {
  m_last_out.clear();
  m_last_err.clear();
  append_capped(&m_last_out, result.console_out);
  append_capped(&m_last_err, result.console_err);
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
      if (e == nullptr) {
        // A guest error without its details is the worker's fault, not the
        // routine's, and not a reason to give the worker up.
        log_error("Internal error in the worker of " + principal + " during " +
                  log_safe(routine.qualified_name) +
                  ": an error result without an error");
        my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
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
      keep_stack_trace(*e, routine);
      // Where it happened, at the innermost frame of the routine's code
      // (exp note 39); the frames: mle_session_state('traceback'/'stack_trace').
      const std::string message = e->message + error_location(*e);
      my_error(ER_LANGUAGE_COMPONENT_RUNTIME, MYF(0), language, message.c_str());
      if (m_console_now) {
        // The warnings go to the session's client, whoever's rights the
        // routine runs with.
        Security_context_handle client = nullptr;
        if (mysql_service_mysql_thd_attributes->get(
                m_thd, "main_security_context", &client))
          client = nullptr;  // a server that does not tell: no source lines
        lines_to_warnings(traceback_for(client), routine.name + " traceback");
      }
      const auto it = m_workers.find(principal);
      if (it != m_workers.end() &&
          (!it->second.worker || !it->second.worker->usable())) {
        // Retired after running out of heap or stack.
        log_warning("The worker of " + display_key(principal) +
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
  if (status == Status::kProtocolError) {
    // Lost, but the reason is a host-side diagnostic: the log has it, the
    // caller gets an internal error.
    ++g_counters.worker_failures;
    log_error("The worker of " + display_key(principal) + " broke the protocol during " +
              log_safe(routine.qualified_name) + " and was killed: " +
              log_safe(why));
    drop_worker(principal);
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
    return;
  }
  if (status != Status::kInternal) {
    ++g_counters.worker_failures;
    if (why.find("out-of-memory") != std::string::npos) ++g_counters.oom_errors;
    log_warning("The worker of " + display_key(principal) + " was lost during " +
                log_safe(routine.qualified_name) + ": " + log_safe(why));
    drop_worker(principal);
    if (status == Status::kKilled && Hooks(m_thd).killed()) return;
    // The caller learns why (the worker died: signal ..., did not stop after
    // KILL ...); the next call of the session gets a new worker.
    engine_error(why);
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
  m_trace = Trace{e, language, routine.name, routine.definer_key};
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

std::string Session::traceback_for(void *sctx) const {
  if (!m_trace) return "";
  // MySQL shows a routine's definition to its definer and to holders of
  // SHOW_ROUTINE (and of a global SELECT, which a component cannot ask
  // about: such a reader sees no source lines here). A library has no
  // definer to compare with.
  bool routine_text = false, other_text = false;
  const auto reader = static_cast<Security_context_handle>(sctx);
  if (reader != nullptr) {
    other_text = mysql_service_global_grants_check->has_global_grant(
        reader, "SHOW_ROUTINE", strlen("SHOW_ROUTINE"));
    routine_text = other_text || m_trace->author.empty();
    MYSQL_LEX_CSTRING user{"", 0}, host{"", 0};
    if (!routine_text &&
        !mysql_service_mysql_security_context_options->get(reader, "priv_user",
                                                           &user) &&
        !mysql_service_mysql_security_context_options->get(reader, "priv_host",
                                                           &host))
      routine_text = account_key({user.str, user.length},
                                 {host.str, host.length}) == m_trace->author;
  }
  return traceback_text(m_trace->error, m_trace->language, m_trace->routine,
                        routine_text, other_text);
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
  if (key == "traceback") {
    if (s == nullptr) return "";
    // For who reads it: the statement's current account.
    Security_context_handle reader = nullptr;
    if (mysql_service_mysql_thd_security_context->get(s->m_thd, &reader))
      reader = nullptr;
    return s->traceback_for(reader);
  }
  if (key == "last_output")
    return s != nullptr ? s->m_last_out + s->m_last_err : "";
  if (key == "stored_functions")
    return count(MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION);
  if (key == "stored_procedures")
    return count(MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE);
  if (key == "stored_programs") return count(0);
  return std::nullopt;
}

long Session::worker_pid() {
  // The current principal's worker (the JVM worker's first, if it has several).
  const std::string who = principal();
  long pid = -1;
  for (const auto &[key, slot] : m_workers)
    if (slot.principal == who && slot.worker && (pid == -1 || slot.kind == WorkerKind::kJvm))
      pid = slot.worker->process().pid();
  return pid;
}

void Session::reset_output(bool out, bool err) {
  if (out) m_out.clear(), m_last_out.clear();
  if (err) m_err.clear(), m_last_err.clear();
}

void Session::clear_state(bool reuse) {
  for (auto &[key, slot] : m_workers)
    engine_release_worker(std::move(slot.worker), slot.kind, reuse ? slot.principal : "",
                          m_mark);
  m_workers.clear();
  m_programs.clear();
  // Everything a later account could read with mle_session_state() (C1).
  m_stack.clear();
  m_trace.reset();
  m_out.clear();
  m_err.clear();
  m_last_out.clear();
  m_last_err.clear();
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
  // The new account's workers: kept unless it is forgotten from now on.
  engine_session_end(m_mark);
  m_mark = engine_session_begin();
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
      log_warning("Options for the worker of " + display_key(principal) + ": " + error);
  }
  return true;
}

// ------------------------------------------------------------- server paths

bool server_private_paths(std::vector<std::string> *paths) {
  // Read once (at INSTALL, or at boot for an installed component): they do
  // not change while mysqld runs, and later engine restarts come from SET
  // GLOBAL, which holds the system variables' lock the reader takes. The
  // first engine start reads them, so later ones never do.
  static std::mutex lock;
  static std::vector<std::string> known;
  static bool done = false;
  std::lock_guard<std::mutex> guard(lock);
  if (!done) {
    MYSQL_THD thd = nullptr;
    // GLOBAL values are read through the current THD. None (a component
    // manifest loads components before there is one): fail closed, workers
    // would see plugin_dir and datadir.
    if (mysql_service_mysql_current_thread_reader->get(&thd) || thd == nullptr)
      return false;
    for (const char *name : {"plugin_dir", "datadir"}) {
      char buffer[PATH_MAX];
      void *value = buffer;
      size_t length = sizeof buffer - 1;
      if (mysql_service_mysql_system_variable_reader->get(
              thd, "GLOBAL", "mysql_server", name, &value, &length) ||
          length == 0) {
        known.clear();
        return false;
      }
      known.emplace_back(static_cast<const char *>(value), length);
    }
    done = true;
  }
  *paths = known;
  return true;
}

// ------------------------------------------------------------- privileges

namespace {
/** CREATE_<LANGUAGE>_ROUTINE for each language name (D-3; exp design note
    63). CREATE_JS_ROUTINE is js_lang's too: LANGUAGE JS needs the same grant
    whichever component runs it. */
constexpr const char *kPrivileges[] = {"CREATE_JAVASCRIPT_ROUTINE", "CREATE_PYTHON_ROUTINE",
                                       "CREATE_JAVA_ROUTINE", "CREATE_JS_ROUTINE",
                                       "CREATE_CPYTHON_ROUTINE"};
/** js_lang's privilege: never unregistered by MLE (the server removes a
    dynamic privilege for every component that uses it). */
constexpr const char *kSharedPrivilege = "CREATE_JS_ROUTINE";

const char *privilege_of(const Routine &r) {
  switch (r.language) {
    case kJavaScript:
      return kPrivileges[0];
    case kPython:
      return kPrivileges[1];
    case kJs:
      return kPrivileges[3];
    case kCPython:
      return kPrivileges[4];
    default:
      return kPrivileges[2];
  }
}
}  // namespace

bool register_privileges() {
  // An already registered privilege (js_lang's) registers as success.
  for (const char *p : kPrivileges)
    if (mysql_service_dynamic_privilege_register->register_privilege(
            p, strlen(p)))
      return true;
  return false;
}

void unregister_privileges() {
  for (const char *p : kPrivileges)
    if (strcmp(p, kSharedPrivilege) != 0)
      (void)mysql_service_dynamic_privilege_register->unregister_privilege(
        p, strlen(p));
}

bool Session::parse_library(unsigned language, const std::string &name,
                            const std::string &body) {
  Routine routine;  // for the worker and the messages
  routine.language = language;
  routine.name = name;
  routine.qualified_name = name;
  // From a routine's SQL the worker is the one waiting for that statement:
  // it takes calls there, not a library to check.
  if (m_calls > 0) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> CREATE LIBRARY cannot run from the SQL of an MLE routine");
    return false;
  }
  // A library is code for the language's routines: the same privilege
  // (D-3) as CREATE FUNCTION / PROCEDURE in that language.
  if (!has_global_privilege(m_thd, privilege_of(routine))) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege_of(routine));
    return false;
  }
  const std::string who = slot_key(principal(), routine.language);
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr) return false;
  mle::ParseLibraryMsg m;
  m.language = worker_language(language);
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
  m_console_now = console_warnings(m_thd);  // this failure's, not the last call's
  report(who, s, result, error, routine, nullptr);
  return false;
}

std::optional<std::string> Session::eval(unsigned language,
                                         const std::string &code) {
  // A snippet in the session's worker for this account, like a routine
  // without CREATE (exp note 39): the language's CREATE privilege, since the
  // same code could be put in a routine.
  Routine routine;
  routine.id = next_routine_id();
  routine.language = language;
  routine.name = "mle_eval";
  routine.qualified_name = "mle_eval";
  routine.definer_key = principal();  // the code is the caller's
  if (!has_global_privilege(m_thd, privilege_of(routine))) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege_of(routine));
    return std::nullopt;
  }
  mle::PrepareMsg &m = routine.prepare;
  m.routine_id = routine.id;
  m.language = worker_language(language);
  m.name = routine.name;
  m.entry = "<eval>";  // the worker evaluates the code and returns its value as text
  m.source = code;
  mle::ParamType text;
  text.tag = mle::Tag::kString;
  text.type_name = "LONGTEXT";
  m.returns = text;

  // Called by a routine's SQL: the code is that routine's author's, and runs
  // where that author's code runs (not in the worker of the account's own
  // code, when the author is another account).
  const std::string who = slot_key(principal(m_running_routine), routine.language);
  Slot *slot = worker_for(who, routine);
  if (slot == nullptr || !prepare_in(who, *slot, routine)) {
    forget(routine);
    return std::nullopt;
  }
  // A function in a SELECT: no statements (they need a stored program's
  // context).
  Hooks hooks(m_thd, nullptr, &routine,
              "mle_eval cannot run SQL: put the code in a procedure to run SQL");
  ResultMsg result;
  std::string error;
  Status s;
  {
    const KillGuard kill(this, slot->worker.get());
    s = slot->worker->call(routine.id, 0, {}, hooks, &result, &error);
  }
  keep_console(result);
  m_console_now = (s != Status::kOk || !result.console_out.empty() ||
                   !result.console_err.empty()) &&
                  console_warnings(m_thd);
  if (m_console_now) console_to_warnings(routine, result);
  if (s != Status::kOk) {
    report(who, s, result, error, routine, nullptr);
    forget(routine);
    return std::nullopt;
  }
  forget(routine);
  // What it printed, then its value (as a REPL shows them); NULL for neither.
  std::string out = result.console_out + result.console_err;
  const bool value =
      result.ret && !result.ret->is_null() && result.ret->tag() == mle::Tag::kString;
  if (value) out += result.ret->as_text();
  if (out.empty() && !value) return std::nullopt;
  return out;
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
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));  // not a grant matter
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
