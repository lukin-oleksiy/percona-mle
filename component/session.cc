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

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string_view>
#include <unordered_set>

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

using mle::Outcome;
using mle::ResultMsg;
using mle::Value;

// ------------------------------------------------------------- libraries

namespace {

std::atomic<uint64_t> g_library_epoch{1};

/** The commands that change no library, no routine and no right to use
    one, by the names the server's query events carry. */
bool leaves_libraries(std::string_view command) {
  static const std::unordered_set<std::string_view> kHarmless = {
      "select", "update", "update_multi", "insert", "insert_select", "delete",
      "delete_multi", "replace", "replace_select", "truncate", "load", "do",
      "call", "set_option", "change_db", "begin", "commit", "rollback",
      "rollback_to_savepoint", "savepoint", "release_savepoint", "prepare",
      "execute", "deallocate_prepare", "empty_query", "lock_tables",
      "unlock_tables", "ha_open", "ha_close", "ha_read", "signal", "resignal",
      "get_diagnostics", "help", "kill", "analyze", "check", "checksum",
      "optimize", "repair", "explain_other", "create_table", "create_index",
      "alter_table", "drop_table", "drop_index", "rename_table", "create_view",
      "drop_view", "create_trigger", "drop_trigger"};
  return command.starts_with("show_") || command.starts_with("xa_") ||
         kHarmless.contains(command);
}

/**
  The version tokens of library bodies (protocol §9.10): a number that two
  different (language, body) pairs under one schema and name never share
  while this component is loaded, also across DROP and CREATE of the
  library. The last body given for each schema and name is kept to compare
  with; numbers come from one counter and are never reused, so forgetting a
  body (the table is bounded) only costs sending it once more.
*/
class LibraryVersions {
 public:
  uint64_t of(const std::string &schema, const std::string &name,
              std::string_view language, std::string_view body) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry &e = m_entries[{schema, name}];
    if (e.version == 0 || e.language != language || e.body != body) {
      m_bytes += body.size();
      m_bytes -= e.body.size();
      e.language.assign(language);
      e.body.assign(body);
      e.version = ++m_last;
    }
    e.used = ++m_clock;
    while (m_entries.size() > 1 &&
           (m_bytes > kMaxBytes || m_entries.size() > kMaxEntries)) {
      auto oldest = m_entries.begin();
      for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (it->second.used < oldest->second.used) oldest = it;
      m_bytes -= oldest->second.body.size();
      m_entries.erase(oldest);
    }
    return m_entries.find({schema, name})->second.version;
  }

 private:
  struct Entry {
    std::string language;
    std::string body;
    uint64_t version = 0;
    uint64_t used = 0;
  };
  static constexpr size_t kMaxBytes = 64ULL << 20;
  static constexpr size_t kMaxEntries = 4096;
  std::mutex m_mutex;
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  size_t m_bytes = 0;
  uint64_t m_last = 0;
  uint64_t m_clock = 0;
};

LibraryVersions g_library_versions;

}  // namespace

uint64_t library_epoch() { return g_library_epoch.load(); }

void library_epoch_changed() { ++g_library_epoch; }

void statement_ended(std::string_view sql_command) {
  if (!leaves_libraries(sql_command)) library_epoch_changed();
}

namespace {

/** Read by every session thread, set at INSTALL and UNINSTALL. */
std::atomic<mysql_thd_store_slot> g_slot{nullptr};

constexpr size_t kConsoleKeep = 64 * 1024;

constexpr char kNoSqlWhileLoading[] = "SQL cannot run while a routine is loaded";

/** Forgets an error the server put in the diagnostics area meanwhile. */
void clear_server_error(MYSQL_THD thd) {
  uint16_t empty = STATUS_DA_EMPTY;
  (void)mysql_service_mysql_thd_attributes->set(thd, "da_status", &empty);
}

/** KILL QUERY / KILL CONNECTION / max_execution_time reached the session. */
bool session_killed(MYSQL_THD thd) {
  uint16_t status = STATUS_SESSION_OK;
  mysql_service_mysql_thd_attributes->get(thd, "thd_status", &status);
  return status != STATUS_SESSION_OK;
}

/** The SQL layer's side of one request: the routine's SQL, the libraries it
    imports, the session's bounds, KILL. */
class Hooks : public mle::CallHooks {
 public:
  /** sql: the request's statements (a CALL of a routine), or nullptr for a
      request that runs no SQL: every action but CLOSE then gets no_sql. */
  explicit Hooks(MYSQL_THD thd, SqlBudget *budget = nullptr,
                 SqlRunner *sql = nullptr, const Routine *routine = nullptr,
                 const char *no_sql = kNoSqlWhileLoading)
      : m_thd(thd),
        m_budget(budget),
        m_sql(sql),
        m_routine(routine),
        m_no_sql(no_sql) {}

  /**
    A database library the routine of this request imports (§9.10),
    answered from the catalog as it is now. A routine may have the
    libraries of its USING clause and the libraries those import. The host
    does not read source code, so it cannot tell which libraries a library
    imports: a routine with a USING clause may have every library that the
    account whose rights it runs with may use (the server checks EXECUTE on
    the library in the security context in force, which is that account's),
    a routine without one has none. Every refusal says the same, whether the
    library exists or not.
  */
  void library(const mle::LibraryMsg &m, mle::LibraryResultMsg *r) override {
    *r = mle::LibraryResultMsg();
    const std::string what = m.schema + "." + m.name;
    const auto refuse = [&] {
      *r = mle::LibraryResultMsg();
      r->message = "library " + what + " is not available to " +
                   (m_routine != nullptr ? m_routine->qualified_name
                                         : std::string("this code"));
      repair_utf8(&r->message);
    };
    if (m_routine == nullptr || m_routine->prepare.imports.empty())
      return refuse();
    // A failed lookup may leave the server's error (a missing library, an
    // MDL timeout, KILL) in the diagnostics area: it is not the routine's,
    // the routine gets r->message. A statement's error kept there goes with
    // it, and the SqlRunner must know.
    const auto lookup_failed = [&] {
      refuse();
      clear_server_error(m_thd);
      if (m_sql != nullptr) m_sql->clear();
    };
    my_h_library h = nullptr;
    if (mysql_service_mysql_library->init(
            m_thd, {m.schema.data(), m.schema.size()},
            {m.name.data(), m.name.size()}, {"", 0}, &h) ||
        h == nullptr)
      return lookup_failed();
    mysql_cstring_with_length body{nullptr, 0}, language{nullptr, 0};
    // The server's init() succeeds for a library that does not exist or
    // that the account may not use; reading its body is what fails.
    if (mysql_service_mysql_library->get_body(h, &body) ||
        mysql_service_mysql_library->get_language(h, &language)) {
      (void)mysql_service_mysql_library->deinit(h);
      return lookup_failed();
    }
    const std::string_view text(body.str == nullptr ? "" : body.str, body.length);
    const std::string_view id =
        worker_language(language_bit({language.str, language.length}));
    // A body that is not UTF-8 text is not sent (§7.1), nor one in a
    // language MLE does not run.
    if (id.empty() || !mle::is_valid_utf8(text)) {
      refuse();
    } else {
      r->language = id;
      r->version = g_library_versions.of(m.schema, m.name, id, text);
      if (m.known_version == r->version) {
        r->status = mle::wire::LibraryStatus::kUnchanged;
      } else {
        r->status = mle::wire::LibraryStatus::kBody;
        r->body.assign(text);
      }
    }
    (void)mysql_service_mysql_library->deinit(h);
  }

  void sql(const mle::SqlMsg &m, mle::SqlResultMsg *r) override {
    sql_into(m, r, nullptr);
  }
  /** SQL from the routine (sql.cc); rows may go straight into the mailbox
      arena. */
  void sql_into(const mle::SqlMsg &m, mle::SqlResultMsg *r,
                mle::DirectReply *direct) override {
    if (m_sql != nullptr) return m_sql->handle(m, r, direct);
    // No statement ids exist here: CLOSE closes nothing, every other action
    // is an ERROR and its id stays CLOSED.
    *r = mle::SqlResultMsg();
    if (m.action == mle::wire::SqlAction::kClose) return;
    sql_error(r, ER_LANGUAGE_COMPONENT_INTERNAL, m_no_sql);
    if (m.action == mle::wire::SqlAction::kExecuteMany) r->failed_row = 0;
  }

  void sql_refused(mle::SqlRefusal why, mle::wire::SqlAction action,
                   uint64_t stmt_id, mle::SqlResultMsg *error) override {
    if (m_sql != nullptr) return m_sql->refused(why, action, stmt_id, error);
    if (action != mle::wire::SqlAction::kClose)
      refusal_error(why, m_no_sql, error);
  }

  /** The bytes of SQL messages whose statements are in progress, over all
      requests of the session (§10.7). */
  bool sql_bytes_reserve(uint64_t bytes) override {
    if (m_budget == nullptr) return true;
    if (bytes > kSqlRequestBytesMax ||
        m_budget->request_bytes > kSqlRequestBytesMax - bytes)
      return false;
    m_budget->request_bytes += bytes;
    return true;
  }
  void sql_bytes_release(uint64_t bytes) override {
    if (m_budget != nullptr) m_budget->request_bytes -= bytes;
  }

  bool killed() override { return session_killed(m_thd); }

 private:
  MYSQL_THD m_thd;
  SqlBudget *m_budget;
  SqlRunner *m_sql;
  const Routine *m_routine;
  const char *m_no_sql;
};

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

class Session::Request {
 public:
  explicit Request(Session *s) : m_session(s) { ++s->m_calls; }
  ~Request() {
    if (--m_session->m_calls == 0) {
      // The outermost request ended: no request runs in a retired worker.
      for (auto &w : m_session->m_retired) engine_release_worker(std::move(w));
      m_session->m_retired.clear();
    }
  }
  Request(const Request &) = delete;
  Request &operator=(const Request &) = delete;

 private:
  Session *m_session;
};

/** KILL QUERY / KILL CONNECTION / max_execution_time interrupt the workers
    at once (each Worker's polling of killed() is the backstop). A worker is
    listed while a top-level request of the session is in progress in it;
    its nested requests are covered by that entry. */
class Session::KillGuard {
 public:
  KillGuard(Session *s, mle::Worker *worker) : m_session(s) {
    if (worker->depth() != 0) return;  // nested: its worker is listed
    const size_t n = s->m_running_n.load();
    m_pushed = n < kMaxRunning;
    if (m_pushed) {
      s->m_running[n].store(worker);
      s->m_running_n.store(n + 1);
    }
    if (!s->m_kill_installed) {
      // Fails if another component's handler is installed (e.g. a V8
      // routine called us): the workers' KILL polling still works.
      s->m_kill_installed = !mysql_service_mysql_thd_kill_handler->set(
          s->m_thd,
          [](MYSQL_THD, uint16_t, void *data) {
            auto *session = static_cast<Session *>(data);
            const size_t count = session->m_running_n.load();
            for (size_t i = 0; i < count && i < kMaxRunning; i++)
              if (mle::Worker *w = session->m_running[i].load()) w->interrupt();
          },
          s);
      m_installed = s->m_kill_installed;
    }
  }
  ~KillGuard() {
    if (m_installed) {
      (void)mysql_service_mysql_thd_kill_handler->set(m_session->m_thd,
                                                      nullptr, nullptr);
      m_session->m_kill_installed = false;
    }
    if (m_pushed) {
      const size_t n = m_session->m_running_n.load() - 1;
      m_session->m_running[n].store(nullptr);
      m_session->m_running_n.store(n);
    }
  }
  KillGuard(const KillGuard &) = delete;
  KillGuard &operator=(const KillGuard &) = delete;

 private:
  Session *m_session;
  bool m_pushed = false;
  /** This guard installed the handler: it is the outermost one. */
  bool m_installed = false;
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

void Session::settle(const std::string &key, mle::Worker *worker) {
  if (worker->usable()) return;
  const auto it = m_workers.find(key);
  if (it != m_workers.end() && it->second.worker.get() == worker)
    drop_worker(key);
}

Session::Slot *Session::worker_for(const std::string &key,
                                   const Routine &routine) {
  const std::string principal = key.substr(0, key.find('\0'));
  const WorkerKind kind = kind_of(routine.language);
  const std::string language = worker_language(routine.language);

  auto it = m_workers.find(key);
  if (it != m_workers.end()) {
    Slot &slot = it->second;
    bool keep = slot.worker && slot.worker->usable();
    // A worker runs its kind's languages, but an installation may lack one
    // of them (a JVM worker without Python): the host sends no other.
    if (keep) {
      const auto &languages = slot.worker->languages();
      if (std::find(languages.begin(), languages.end(), language) ==
          languages.end()) {
        engine_error("incompatible MLE worker: it does not run the language '" +
                     language + "'");
        return nullptr;
      }
    }
    // A worker whose principal names an account that was dropped or renamed
    // is retired: an account of that name is another one now (§1).
    if (keep && slot.worker->depth() == 0) {
      const uint64_t forgets = engine_forgets();
      if (forgets != slot.forgets) {
        if (engine_account_forgotten(slot.principal, slot.forgets)) {
          log_info("The worker of " + display_key(key) +
                   " was retired: an account of its principal was dropped or "
                   "renamed.");
          keep = false;
        } else {
          slot.forgets = forgets;
        }
      }
    }
    if (!keep) {
      drop_worker(key);
      it = m_workers.end();
    }
  }
  // A nested request: the worker serves a top-level request of this session
  // and has its options.
  if (it != m_workers.end() && it->second.worker->depth() != 0)
    return &it->second;

  // A top-level request follows. Nothing is sent for a session that is
  // killed or whose statement is out of time (§11.1): the server reports it.
  if (session_killed(m_thd)) return nullptr;

  if (it == m_workers.end()) {
    const uint64_t forgets = engine_forgets();
    AcquireError why = AcquireError::kNone;
    std::string error;
    // A worker kept from an earlier session of this principal (warm, RESET
    // when it was given back), else a new one from the pool.
    std::unique_ptr<mle::Worker> worker =
        engine_reuse_worker(principal, kind, language, max_workers());
    Hooks hooks(m_thd);
    if (!worker)
      worker = engine_new_worker(kind, language, max_workers(), spin_us(),
                                 interrupt_grace_ms(), &hooks, &why, &error);
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
      case AcquireError::kIncompatible:
        // Not the worker's fault and not the routine's: the installed
        // worker and this component do not fit together.
        log_error("The " + std::string(kind_name(kind)) + " worker cannot serve " +
                  routine.display_language() + " for " + display_key(key) + ": " +
                  log_safe(error) +
                  ". The MLE packages installed in mle.dir do not match component_mle.");
        engine_error(error + " (the installed MLE worker does not match the server's MLE component)");
        return nullptr;
      case AcquireError::kFailed:
        log_error("No worker for " + display_key(key) + ": " + log_safe(error));
        my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
        return nullptr;
    }
    Slot &slot = m_workers[key];
    slot.worker = std::move(worker);
    slot.principal = principal;
    slot.kind = kind;
    slot.forgets = forgets;
    it = m_workers.find(key);
  }

  Slot &slot = it->second;
  slot.worker->set_timing(spin_us(), interrupt_grace_ms());
  // Routines that went away while their DROP could not be sent.
  while (!slot.undropped.empty()) {
    std::string error;
    const Outcome o = slot.worker->drop(slot.undropped.back(), &error);
    if (o == Outcome::kRefused) break;
    slot.undropped.pop_back();
    if (!slot.worker->usable()) break;
  }
  if (slot.worker->usable() && options_in(key, slot, routine)) return &slot;
  // The worker was lost on the way (or retired for an INTERNAL answer).
  mle::Worker *const worker = slot.worker.get();
  if (!worker->usable()) {
    ++g_counters.worker_failures;
    log_warning("The worker of " + display_key(key) + " was lost: " +
                log_safe(worker->failure()));
    drop_worker(key);
  }
  return nullptr;
}

bool Session::options_in(const std::string &key, Slot &slot,
                         const Routine &routine) {
  mle::Worker &worker = *slot.worker;
  const std::string zone = session_time_zone(m_thd);
  if (worker.options_in_force() && zone == slot.zone &&
      m_settings_version == slot.settings_version)
    return true;
  mle::OptionsMsg options;
  options.time_zone = options_time_zone(zone);
  if (options.time_zone.empty()) {
    // A zone the protocol cannot express: nothing is sent.
    my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0), routine.display_language(),
             zone.c_str());
    return false;
  }
  // The complete set: a setting that is not listed has its default.
  for (const auto &[name, value] : m_settings)
    options.settings.push_back({name, value});
  if (worker.options_in_force() && options == slot.options) {
    slot.zone = zone;
    slot.settings_version = m_settings_version;
    return true;
  }
  mle::Error refused;
  std::string error;
  switch (worker.options(options, &refused, &error)) {
    case Outcome::kOk:
      slot.options = std::move(options);
      slot.zone = zone;
      slot.settings_version = m_settings_version;
      return true;
    case Outcome::kGuestError:
      // The options in force are unchanged (possibly none) and the worker
      // lives on; the statement fails. Which setting it was is told by the
      // Error's language, never by its text.
      if (refused.language == mle::wire::kLanguageTimeZone) {
        my_error(ER_LANGUAGE_COMPONENT_TIMEZONE, MYF(0),
                 routine.display_language(), zone.c_str());
      } else {
        repair_utf8(&refused.message);
        my_error(ER_LANGUAGE_COMPONENT, MYF(0),
                 ("MLE-Option> " + refused.message).c_str());
      }
      return false;
    case Outcome::kRefused:
      log_error("Options for the worker of " + display_key(key) +
                " were not sent: " + log_safe(error));
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return false;
    default:
      break;
  }
  // INTERNAL (the worker is retired for it), or the worker is lost.
  if (worker.usable()) {
    log_error("The worker of " + display_key(key) +
              " failed to take its options: " + log_safe(error));
  }
  if (!worker.usable()) {
    engine_error(worker.failure());
  } else {
    my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
  }
  return false;
}

bool Session::interrupted(Outcome outcome, const mle::Worker &worker) const {
  switch (outcome) {
    case Outcome::kInterrupted:
    case Outcome::kKilled:
      return true;
    case Outcome::kRefused:
    case Outcome::kIncompatible:
      return false;  // nothing was sent
    default:
      // Whatever the RESULT says, and also when the worker had to be ended.
      return worker.interrupted() || session_killed(m_thd);
  }
}

bool Session::prepare_in(const std::string &key, Slot &slot, Routine &routine) {
  mle::Worker *const worker = slot.worker.get();
  if (worker->is_prepared(routine.id)) return true;
  Hooks hooks(m_thd, &m_budget, nullptr, &routine);
  ResultMsg result;
  std::string error;
  Outcome outcome;
  // The epoch as it is when the request is sent.
  routine.prepare.library_epoch = library_epoch();
  {
    const KillGuard kill(this, worker);
    outcome = worker->prepare(routine.prepare, hooks, &result, &error);
  }
  keep_console(result);
  findings_to_warnings(routine, result);
  const bool stopped = interrupted(outcome, *worker);
  if (outcome == Outcome::kOk && !stopped) {
    settle(key, worker);
    m_programs[routine.id] = routine.sp_type;
    return true;
  }
  m_console_now = console_warnings(m_thd);  // this failure's, not the last call's
  report(key, worker, outcome, stopped, result, error, routine, nullptr);
  return false;
}

mle::Worker *Session::prepared_worker(const std::string &key,
                                      Routine &routine) {
  for (int attempt = 0; attempt < 2; attempt++) {
    Slot *slot = worker_for(key, routine);
    if (slot == nullptr) return nullptr;
    mle::Worker *const worker = slot->worker.get();
    if (!prepare_in(key, *slot, routine)) return nullptr;
    // A worker can ask to be retired with the RESULT of the PREPARE: then
    // once more, in a new one.
    if (worker->usable()) return worker;
  }
  engine_error("the worker was retired while the routine was loaded");
  return nullptr;
}

// ------------------------------------------------------------- requests

bool Session::parse(Routine &routine) {
  if (!check_create_privilege(m_thd, routine)) return false;
  const Request request(this);
  const std::string key = slot_key(principal(&routine), routine.language);
  Slot *slot = worker_for(key, routine);
  if (slot == nullptr) return false;
  // CREATE: the worker also checks the code; what it finds becomes the
  // statement's warnings.
  enum_sql_command command = SQLCOM_END;
  (void)mysql_service_mysql_thd_attributes->get(m_thd, "sql_command_id", &command);
  routine.prepare.check =
      command == SQLCOM_CREATE_PROCEDURE || command == SQLCOM_CREATE_SPFUNCTION;
  const bool ok = prepare_in(key, *slot, routine);
  routine.prepare.check = false;
  return ok;
}

void Session::findings_to_warnings(const Routine &routine,
                                   const ResultMsg &result) {
  for (const std::string &finding : result.findings) {
    const std::string message = std::string(routine.display_language()) + "> " +
                                routine.name + ", " + finding;
    mysql_service_mysql_runtime_warning->emit(m_thd, ER_LANGUAGE_COMPONENT,
                                              message.c_str());
  }
}

bool Session::execute(Routine &routine) {
  m_stack.clear();  // Oracle: the trace of the last call only
  m_trace.reset();
  std::vector<Value> args = std::move(m_args);  // reused; a nested call allocates
  if (!routine.read_args(&args)) return false;
  const Request request(this);
  const std::string key =
      slot_key(principal(&routine), routine.language);  // a copy: nested calls change it
  // Its slot may go while the call runs (a nested request can lose the
  // worker); the worker itself lives until this request ends.
  mle::Worker *const worker = prepared_worker(key, routine);
  if (worker == nullptr) return false;

  // Whose code runs, for what it evaluates with mle_eval(): restored when
  // the call returns (a nested call has its own).
  struct Running {
    const Routine *&slot;
    const Routine *saved;
    ~Running() { slot = saved; }
  } running{m_running_routine, m_running_routine};
  m_running_routine = &routine;

  // The call's statements; closed when it returns.
  SqlRunner sql(m_thd, &m_budget);
  Hooks hooks(m_thd, &m_budget, &sql, &routine);
  ResultMsg result;
  std::string error;
  Outcome outcome;
  {
    // mle_threads: counted while the call runs, released if it throws too.
    struct Counted {
      Counted() { raise_max(g_counters.threads_max, ++g_counters.threads); }
      ~Counted() { --g_counters.threads; }
    } counted;
    const KillGuard kill(this, worker);
    outcome = worker->call(routine.id, library_epoch(), args, hooks, &result, &error);
  }
  keep_console(result);
  // An interrupted call is reported as interrupted even if the routine
  // ended normally (an inner SLEEP returned early and the worker finished
  // before the interrupt reached it).
  const bool stopped = interrupted(outcome, *worker);
  const bool ok = outcome == Outcome::kOk && !stopped;
  // mle.console = WARNINGS: read only when there is something to show.
  m_console_now = (!ok || !result.console_out.empty() ||
                   !result.console_err.empty()) &&
                  console_warnings(m_thd);
  if (m_console_now) console_to_warnings(routine, result);
  if (ok) {
    sql.call_succeeded();
    settle(key, worker);
  } else {
    report(key, worker, outcome, stopped, result, error, routine, &args, &sql);
  }
  args.clear();
  m_args = std::move(args);
  return ok && routine.store_results(result);
}

void Session::forget(const Routine &routine) {
  m_programs.erase(routine.id);
  std::vector<std::string> lost;
  for (auto &[key, slot] : m_workers) {
    if (!slot.worker || !slot.worker->usable() ||
        !slot.worker->is_prepared(routine.id))
      continue;
    std::string error;
    // A DROP that cannot be sent now (the worker is busy where no nested
    // request fits) goes before the worker's next top-level request.
    if (slot.worker->drop(routine.id, &error) == Outcome::kRefused)
      slot.undropped.push_back(routine.id);
    if (!slot.worker->usable()) lost.push_back(key);
  }
  for (const std::string &key : lost) {
    log_warning("The worker of " + display_key(key) + " was lost: " +
                log_safe(m_workers[key].worker->failure()));
    drop_worker(key);
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



void Session::report(const std::string &key, mle::Worker *worker,
                     Outcome outcome, bool stopped, const ResultMsg &result,
                     const std::string &error, const Routine &routine,
                     const std::vector<Value> *args, SqlRunner *sql) {
  const mle::Error *e = result.error ? &*result.error : nullptr;
  const bool lost = outcome == Outcome::kDied || outcome == Outcome::kKilled ||
                    outcome == Outcome::kViolation ||
                    outcome == Outcome::kTimedOut;
  const std::string why =
      !error.empty() ? error : e != nullptr ? e->message : "no details";
  // The worker's part first: one that is lost or was retired leaves its
  // slot, whatever the client is told.
  if (lost) {
    ++g_counters.worker_failures;
    if (why.find("out-of-memory") != std::string::npos) ++g_counters.oom_errors;
    if (outcome == Outcome::kViolation)
      log_error("The worker of " + display_key(key) +
                " broke the protocol during " + log_safe(routine.qualified_name) +
                " and was ended: " + log_safe(why));
    else
      log_warning("The worker of " + display_key(key) + " was lost during " +
                  log_safe(routine.qualified_name) + ": " + log_safe(why));
  } else if (!worker->usable()) {
    // Retired after the request: it reported that it must not be used
    // again (out of heap or stack, damaged state).
    log_warning("The worker of " + display_key(key) + " was retired after " +
                log_safe(routine.qualified_name) + ": " +
                log_safe(worker->failure()));
  }
  settle(key, worker);

  // To the client an interrupted request is interrupted in every case:
  // whatever the RESULT says, and also when the worker had to be ended
  // (§11.1 step 4).
  if (stopped) {
    if (sql != nullptr) sql->clear();
    // A KILL or max_execution_time: the SQL layer reports it.
    if (session_killed(m_thd)) return;
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    return;
  }

  // The error a routine ends with (§10.8); only GUEST_ERROR looks at the
  // Error's SQL fields.
  if (outcome == Outcome::kGuestError && e != nullptr) {
    // 1. An ERROR the host sent during this top-level request, handed back
    //    by its id: the client gets that error. It is unchanged if it is
    //    still the one in the diagnostics area; an older one is raised
    //    again with its number and SQLSTATE.
    const mle::HeldError *held =
        e->sql_error_id != 0 ? worker->held_error(e->sql_error_id) : nullptr;
    if (held != nullptr) {
      if (sql != nullptr && sql->propagates(*held)) return;
      if (signal_error(m_thd, held->sql_errno, held->sqlstate, held->message))
        return;
    } else {
      // 2. The routine raises its own SQL error.
      if (sql != nullptr) sql->clear();
      if (signal_error(m_thd, e->sql_errno, e->sqlstate, e->message)) return;
    }
    // 3. A routine error (also where the server lets no error be raised by
    //    number: in a stored function or trigger).
  }
  if (sql != nullptr) sql->clear();

  switch (outcome) {
    case Outcome::kGuestError:
    case Outcome::kExhausted: {
      if (outcome == Outcome::kExhausted) ++g_counters.oom_errors;
      if (e == nullptr) break;  // cannot be: the Worker checks every RESULT
      std::string text = e->message;
      repair_utf8(&text);
      if (e->language == mle::wire::kLanguageType) {
        std::string message = "MLE-Type> " + text;
        if (args != nullptr)
          message += " in '" + routine.call_text(*args) + "'";
        my_error(ER_LANGUAGE_COMPONENT, MYF(0), message.c_str());
        return;
      }
      const char *language = e->language.empty()
                                 ? routine.display_language()
                                 : display_language(e->language);
      keep_stack_trace(*e, routine);
      // Where it happened, at the innermost frame of the routine's code;
      // the frames: mle_session_state('traceback' / 'stack_trace').
      const std::string message = text + error_location(*e);
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
      return;
    }
    case Outcome::kRefused:
      // The host did not send the request: nesting too deep, a routine
      // larger than a message may be.
      engine_error(why);
      return;
    case Outcome::kViolation:
      // The reason is a host-side diagnostic: the log has it.
      my_error(ER_LANGUAGE_COMPONENT_INTERNAL, MYF(0));
      return;
    case Outcome::kDied:
    case Outcome::kKilled:
    case Outcome::kTimedOut:
      // The caller learns why (the worker died: signal ...); the next call
      // of the session gets a new worker.
      engine_error(why);
      return;
    default:
      break;
  }
  // INTERNAL from PREPARE, CALL or PARSE_LIBRARY: the request fails with an
  // internal error; the worker stays in use unless it asked to be retired.
  log_error("Internal error in the worker of " + display_key(key) + " during " +
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
  // Everything a later account could read with mle_session_state().
  m_stack.clear();
  m_trace.reset();
  m_out.clear();
  m_err.clear();
  m_last_out.clear();
  m_last_err.clear();
  m_settings.clear();
  ++m_settings_version;
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

bool Session::set_settings(
    const std::vector<std::pair<std::string, std::string>> &settings) {
  if (has_programs()) return false;
  for (const auto &[name, value] : settings) {
    if (value.empty())
      m_settings.erase(name);
    else
      m_settings[name] = value;
  }
  // The workers get them with OPTIONS before their next top-level request.
  ++m_settings_version;
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
  // it takes nested calls there, not a library to check.
  if (m_calls > 0) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> CREATE LIBRARY cannot run from the SQL of an MLE routine");
    return false;
  }
  // A library is code for the language's routines: the same privilege as
  // CREATE FUNCTION / PROCEDURE in that language.
  if (!has_global_privilege(m_thd, privilege_of(routine))) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), privilege_of(routine));
    return false;
  }
  mle::ParseLibraryMsg m;
  m.language = worker_language(language);
  m.name = name;
  m.body = body;
  // Source code crosses as it is or not at all (protocol §7.1).
  if (!mle::is_valid_utf8(m.body)) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> The library's code is not valid UTF-8 text; create it over "
             "a connection whose character set is utf8mb4");
    return false;
  }
  repair_utf8(&m.name);
  const Request request(this);
  const std::string key = slot_key(principal(), routine.language);
  Slot *slot = worker_for(key, routine);
  if (slot == nullptr) return false;
  mle::Worker *const worker = slot->worker.get();
  Hooks hooks(m_thd);
  ResultMsg result;
  std::string error;
  Outcome outcome;
  {
    const KillGuard kill(this, worker);
    outcome = worker->parse_library(m, hooks, &result, &error);
  }
  const bool stopped = interrupted(outcome, *worker);
  if (outcome == Outcome::kOk && !stopped) {
    settle(key, worker);
    return true;
  }
  m_console_now = console_warnings(m_thd);  // this failure's, not the last call's
  report(key, worker, outcome, stopped, result, error, routine, nullptr);
  return false;
}

std::optional<std::string> Session::eval(unsigned language,
                                         const std::string &code) {
  // A snippet in the session's worker for this account, like a routine
  // without CREATE: the language's CREATE privilege, since the same code
  // could be put in a routine.
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
  if (!mle::is_valid_utf8(code)) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> The code is not valid UTF-8 text");
    return std::nullopt;
  }
  mle::PrepareMsg &m = routine.prepare;
  m.routine_id = routine.id;
  m.language = worker_language(language);
  m.name = routine.name;
  m.entry = "<eval>";  // the worker evaluates the code and returns its value as text
  m.source = code;
  // The current schema while it runs; a session without one names a schema
  // that holds no libraries.
  mysql_cstring_with_length schema{nullptr, 0};
  if (!mysql_service_mysql_thd_attributes->get(m_thd, "schema", &schema) &&
      schema.str != nullptr && schema.length > 0)
    m.schema.assign(schema.str, schema.length);
  if (m.schema.empty() || !mle::is_valid_utf8(m.schema))
    m.schema = "information_schema";
  routine.schema = m.schema;
  mle::ParamType text;
  text.tag = mle::Tag::kString;
  text.type_name = "LONGTEXT";
  m.returns = text;

  // Directly from the session both accounts of the principal are the
  // session's. Called by a routine's SQL, the code is what that routine
  // made: it runs where that routine's owner's code runs with these rights
  // (not in the worker of the account's own code, when the owner is
  // another account).
  const Request request(this);
  const std::string key =
      slot_key(principal(m_running_routine), routine.language);
  // Dropped however it ends: the id is used once.
  struct Dropped {
    Session *session;
    const Routine &routine;
    ~Dropped() { session->forget(routine); }
  } dropped{this, routine};
  mle::Worker *const worker = prepared_worker(key, routine);
  if (worker == nullptr) return std::nullopt;
  // A function in a SELECT: no statements (they need a stored program's
  // context).
  Hooks hooks(m_thd, &m_budget, nullptr, &routine,
              "mle_eval cannot run SQL: put the code in a procedure to run SQL");
  ResultMsg result;
  std::string error;
  Outcome outcome;
  {
    const KillGuard kill(this, worker);
    outcome = worker->call(routine.id, library_epoch(), {}, hooks, &result, &error);
  }
  keep_console(result);
  const bool stopped = interrupted(outcome, *worker);
  const bool ok = outcome == Outcome::kOk && !stopped;
  m_console_now = (!ok || !result.console_out.empty() ||
                   !result.console_err.empty()) &&
                  console_warnings(m_thd);
  if (m_console_now) console_to_warnings(routine, result);
  if (!ok) {
    report(key, worker, outcome, stopped, result, error, routine, nullptr);
    return std::nullopt;
  }
  settle(key, worker);
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
