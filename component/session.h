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

#ifndef COMPONENTS_MLE_COMPONENT_SESSION_H
#define COMPONENTS_MLE_COMPONENT_SESSION_H

/**
  @file component/session.h

  The MLE state of a client session (thd_store slot): its workers, what the
  session's routines make mysqld hold through SQL, the session's language
  settings, and the console output. Created at the session's first MLE use,
  freed with the session.

  Workers and principals (docs/protocol.md §1). A worker has one security
  principal for its whole life: the account that owns the code and the
  account whose rights the code runs with, both as priv_user@priv_host. The
  server tells a routine's definer but not its SQL SECURITY, so the pair is
  (definer, account of the security context in force): for a routine that
  runs with its definer's rights both are the definer; for one that runs
  with the invoker's rights it is (definer, invoker); for mle_eval() and
  CREATE LIBRARY both are the account of the context in force. A session
  holds one worker per principal and worker kind. At CREATE the code is the
  creator's, and the server does not tell the definer then: the routine is
  checked in the worker of the account of the context in force. Code that
  a routine's SQL submits (a routine it creates, mle_eval) has that
  routine's principal. The principal is kept per routine and kind for the
  account of the security context (compared byte for byte), and forgotten
  when an account is dropped, renamed or created. Accounts that change
  (accounts.hpp): a worker whose principal names an account that was
  dropped or renamed is retired before its next top-level request; one
  that recorded at its start an account of its principal that did not
  exist (an owner that does not exist, a session that outlived its
  account) is retired so when an account may have been created, and is
  never kept for another session.

  Options (§9.8, Appendix A: Oracle's rule). A session has one set of fixed
  options: its time zone and settings as they are when a worker first takes
  them, before the session's first PREPARE, CALL or PARSE_LIBRARY. They
  hold until mle_session_reset() (or COM_CHANGE_USER), whatever the session
  does to its zone meanwhile, or until mle_set_session_state() replaces
  them while no id is prepared in the session's workers. Every worker the
  session uses gets them once, when the session takes it (new, or kept and
  RESET) or after they were replaced: OPTIONS goes only to a worker with
  nothing prepared, and a request compares two numbers at most.

  Only the session's own thread uses it (deinit gets the THD explicitly; the
  KILL handler touches only the list of running workers). UNINSTALL is
  refused while sessions exist: the server does not free objects of an
  unregistered thd_store slot.
*/

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "mle/codec.h"
#include "engine.h"
#include "mle/worker.h"
#include "routine.h"
#include "sql.h"

namespace mle_component {

class Session {
 public:
  ~Session();

  static bool register_slot();
  static void unregister_slot();
  /** Sessions that used MLE and are still connected. */
  static long count();

  /** The session of the current thread; created when create is set. */
  static Session *current(bool create);
  /** The session of thd, or nullptr. */
  static Session *of(MYSQL_THD thd);

  /** CREATE (and loading into the session's routine cache): the worker of
      the current principal compiles the routine (syntax errors -> 6113).
      At CREATE the worker also checks the code: its findings become the
      statement's warnings. */
  bool parse(Routine &routine);
  /** mle_eval(language, code): what the snippet printed and its value; nullopt
      after reporting an error. */
  std::optional<std::string> eval(unsigned language, const std::string &code);
  /** CREATE LIBRARY: syntax check in the account's worker; false after
      reporting the error (6113 "JavaScript> SyntaxError: ..."). */
  bool parse_library(unsigned language, const std::string &name,
                     const std::string &body);
  /** Runs the routine for the current principal. False after reporting
      the error. */
  bool execute(Routine &routine);
  /** The routine is gone (ALTER, DROP, cache eviction): workers drop it. */
  void forget(const Routine &routine);

  /** mle_session_state(key): is_active, stdout, stderr, stack_trace,
      stored_functions, stored_procedures, stored_programs; nullopt for an
      unknown key. Without a session (s == nullptr) the empty state. */
  static std::optional<std::string> state(const Session *s,
                                          const std::string &key);
  /** mle_session_reset('stdout' | 'stderr' | 'output'). */
  void reset_output(bool out, bool err);
  /** mle_session_reset(): workers closed (the next call gets a new one),
      programs, stack trace and settings forgotten. False while a routine
      of this session runs (it would destroy its worker). */
  bool reset();
  /** COM_CHANGE_USER: the session's state belongs to the old account; its
      workers go back to the engine for that account's later sessions. */
  void account_changed();
  /** Whether this session holds a worker (SET GLOBAL of a worker-shaping
      variable cannot wait for it). */
  bool holds_worker() const {
    for (const auto &[principal, slot] : m_workers)
      if (slot.worker) return true;
    return false;
  }
  /** Programs prepared in this session's workers (Oracle: "active"). */
  bool has_programs() const { return !m_programs.empty(); }
  /** The pid of the current principal's worker, or -1 (debug UDF). */
  /** The current principal's worker of kind (none: the JVM worker's, else any); -1 if none. */
  long worker_pid(std::optional<WorkerKind> kind = std::nullopt);
  /** Whether mle_set_session_state() may replace the session's options
      now (protocol §9.8, Appendix A): no request of it in progress, no
      program cached, and no id prepared in its workers once the ids of
      mle_eval() code and of routines that are gone were dropped (which
      this does first). */
  bool options_replaceable();
  /** mle_set_session_state(): settings by their OPTIONS names (protocol
      Appendix B.2), values checked by the caller; an empty value is the
      setting's default. False unless options_replaceable(). They become
      part of the session's fixed options at its next request: options
      fixed earlier end, and each worker of the session gets the new ones
      before its next request. */
  bool set_settings(
      const std::vector<std::pair<std::string, std::string>> &settings);

 private:
  /** A worker of the session: one per principal and worker kind. */
  struct Slot {
    std::unique_ptr<mle::Worker> worker;
    std::string principal;
    WorkerKind kind = WorkerKind::kJvm;
    /** engine_forgets() when no account of the principal was known to be
        dropped, renamed or created. */
    uint64_t forgets = 0;
    /** An account of the principal did not exist when the worker started
        (protocol §1): retired at a creation, never kept. */
    bool unknown_account = false;
    /** Language bits checked against the worker's READY. */
    unsigned languages_checked = 0;
    /** m_options_generation when the worker took the session's options. */
    uint64_t options_generation = 0;
    /** timing_generation() when the worker got mle.spin_us and
        mle.interrupt_grace_ms. */
    uint64_t timing_generation = 0;
  };
  /** The key as the error log shows it: "'u'@'h' (v8)". */
  static std::string display_key(const std::string &key);

  explicit Session(MYSQL_THD thd);

  /** A worker's principal (§1) for a routine and a worker kind under the
      security context in force: the account of the context when the
      routine's definer is that account (or there is no routine), else
      "<account> running code of <definer>". Code of different owners never
      shares a worker, where it could change the language's shared objects
      (JSON.parse, builtins, statics) under the other's code. Immutable. */
  struct Principal {
    // What it was computed for (the cache key).
    uint64_t routine_id = 0;
    WorkerKind kind = WorkerKind::kJvm;
    /** The server told the context's account (else principal is "?"). */
    bool told = false;
    std::string user, host;  // the context's priv_user and priv_host
    uint64_t forgets = 0;    // engine_forgets() then
    // What it is.
    std::string principal;
    /** The key of its slot in m_workers: principal, '\0', the kind's name
        ('\0' is not in account names). */
    std::string slot;
    std::string rights_key;  // account_key(user, host)
    /** The account that owns the code when it is another one; empty
        owner_key: the rights account owns it. */
    std::string owner_user, owner_host, owner_key;
    /** Where its slot was found last, valid while m_slot_erasures equals
        slot_erasures (a map's element stays where it is until erased). */
    mutable std::map<std::string, Slot>::iterator slot_at;
    mutable uint64_t slot_erasures = 0;
  };
  /** The principal for the routine (nullptr: code the session submits) and
      the language's worker kind; kept per routine, kind and the context's
      account, until an account is dropped, renamed or created. The
      reference is valid while a request of the session is in progress, or
      until the next call. */
  const Principal &principal(const Routine *routine, unsigned language);
  /** Whether every account of the principal exists now, as the incarnation
      the session knows (protocol §1). */
  bool accounts_exist(const Principal &who) const;
  /** Gives the slot's worker back to the engine: kept for its principal
      when reuse allows and no account of it was unknown at its start. */
  void release(Slot &slot, bool reuse);
  /** Releases the workers (kept for their principals if reuse) and forgets
      programs, console, stack trace, settings. */
  void clear_state(bool reuse);
  /** The principal's worker, started if needed and ready for a request: at
      top level with the session's options in force. nullptr after reporting
      the error (or for a session that is killed: the server reports it). */
  Slot *worker_for(const Principal &who, const Routine &routine);
  /** OPTIONS with the session's fixed options when the slot's worker has
      none in force; the first OPTIONS a worker of the session answers OK
      fixes them. False after reporting. */
  bool options_in(const std::string &key, Slot &slot, const Routine &routine);
  /** PREPAREs the routine in the slot's worker unless it is prepared there. */
  bool prepare_in(const std::string &key, Slot &slot, Routine &routine);
  /** The principal's worker with the routine prepared in it; nullptr after
      reporting the error. */
  mle::Worker *prepared_worker(const Principal &who, Routine &routine);
  /** What a request's outcome is for the client when an interrupt may have
      been requested (§11.1 step 4). */
  bool interrupted(mle::Outcome outcome, const mle::Worker &worker) const;
  /** Reports a failed request; drops the worker if it is gone. */
  void report(const std::string &key, mle::Worker *worker, mle::Outcome outcome,
              bool interrupted, const mle::ResultMsg &result,
              const std::string &error, const Routine &routine,
              const std::vector<mle::Value> *args, SqlRunner *sql = nullptr);
  /** After a request: a worker that was retired or ended leaves its slot. */
  void settle(const std::string &key, mle::Worker *worker);
  void keep_console(const mle::ResultMsg &result);
  void keep_stack_trace(const mle::Error &error, const Routine &routine);
  /** Takes the worker out of the session: ended, never kept. */
  void drop_worker(const std::string &key);
  /** A request of this session, from before its worker is chosen until its
      outcome is reported: a worker dropped meanwhile (by a nested request)
      lives until the outermost one ends, for outer requests still run in
      it. */
  class Request;
  /** The KILL handler on the session while interruptible requests run. The
      server keeps one handler per session and refuses to replace it, so it
      is installed once for the outermost request, and interrupts every
      worker that has a top-level request of this session in progress (a
      chain of calls can pass through several workers). It is installed only
      when a request waits long enough for the host to sleep (install_kill_
      handler()): setting it takes the server's lock on the session twice,
      which a short call does not need (the host looks for a kill at every
      bounded sleep and after every message, §11.1). */
  class KillGuard;
  static constexpr size_t kMaxRunning = 64;
  std::array<std::atomic<mle::Worker *>, kMaxRunning> m_running{};
  std::atomic<size_t> m_running_n{0};
  /** A KillGuard is in place (the outermost one owns the handler). */
  bool m_kill_guarded = false;
  bool m_kill_installed = false;
  /** Installing was tried for the outermost request (and failed: another
      component's handler is set). */
  bool m_kill_tried = false;

 public:
  /** A request of the session waits long for its worker: the KILL handler
      is installed for the rest of the outermost request (KillGuard). */
  void install_kill_handler();

 private:
  /** Requests of this session in progress (nested: routine -> SQL ->
      routine). */
  size_t m_calls = 0;
  /** The routine of the innermost call in progress (nullptr: none). */
  const Routine *m_running_routine = nullptr;
  /** What the requests in progress hold through SQL: all nesting levels and
      all workers, one budget (§10.7). */
  SqlBudget m_budget;
  std::vector<std::unique_ptr<mle::Worker>> m_retired;

  MYSQL_THD m_thd;
  /** engine_session_begin(); renewed at COM_CHANGE_USER. */
  uint64_t m_mark;
  std::map<std::string, Slot> m_workers;
  /** Argument buffer of the top-level call, reused. */
  std::vector<mle::Value> m_args;
  /** Principals computed so far (principal()); pruned only while no
      request of the session is in progress. */
  std::vector<std::unique_ptr<const Principal>> m_principals;
  /** Incremented when a slot leaves m_workers (Principal::slot_at). */
  uint64_t m_slot_erasures = 1;
  /** Incremented when the session's options are replaced: a worker that
      took older ones takes the new ones before its next request. */
  uint64_t m_options_generation = 1;
  /** The session's settings that are not at their default, by name: what
      mle_set_session_state() gave for the options to be fixed next. */
  std::map<std::string, std::string> m_settings;
  /** The session's fixed options (§9.8), and the session's zone as the
      server named it when they were fixed (for messages). */
  std::optional<mle::OptionsMsg> m_fixed;
  std::string m_fixed_zone;
  /** The session's current zone, for TIMESTAMP values. */
  ZoneCache m_zones;
  std::string m_out;
  std::string m_err;
  /** Oracle's stack_trace: the frames of the last call's runtime error. */
  std::string m_stack;
  /** The same error, kept to show as a traceback in its language's form.
      Its source lines are for those who may see the code: decided for each
      reader (traceback_for). */
  struct Trace {
    mle::Error error;
    std::string language;
    /** The routine's name (its frames' file) and its code's author
        (account_key(); empty: not known, e.g. while it is created). */
    std::string routine;
    std::string author;
  };
  std::optional<Trace> m_trace;
  /** The traceback as the account of sctx may see it (nullptr: an unknown
      reader, no source lines). */
  std::string traceback_for(void *sctx) const;
  /** What the last call printed (mle_session_state('last_output')). */
  std::string m_last_out;
  std::string m_last_err;
  /** mle.console = WARNINGS for the call being reported (read once per call). */
  bool m_console_now = false;
  /** mle.console: the lines a call printed, as warnings. */
  void console_to_warnings(const Routine &routine, const mle::ResultMsg &result);
  void lines_to_warnings(const std::string &text, const std::string &prefix);
  /** CREATE: what the worker's check found, as warnings. */
  void findings_to_warnings(const Routine &routine, const mle::ResultMsg &result);
  /** Routine id -> its type (stored_functions / stored_procedures). */
  std::map<uint64_t, uint16_t> m_programs;
};

/** CREATE FUNCTION/PROCEDURE needs CREATE_<LANGUAGE>_ROUTINE too;
    false after reporting the missing privilege. Other statements pass. */
bool check_create_privilege(MYSQL_THD thd, const Routine &routine);

/** CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE,
    CREATE_JS_ROUTINE (shared with js_lang), CREATE_CPYTHON_ROUTINE. */
bool register_privileges();
void unregister_privileges();

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_SESSION_H
