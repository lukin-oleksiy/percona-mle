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
  holds one worker per principal and worker kind. A worker whose principal
  names an account that was dropped or renamed is retired before its next
  top-level request.

  Options (§9.8). Before every top-level PREPARE, CALL or PARSE_LIBRARY the
  session's time zone and settings are compared with the options in force
  in that worker, and OPTIONS is sent when they differ or none are in force
  (a new worker, a kept one after its RESET).

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
  long worker_pid();
  /** mle_set_session_state(): language settings by their OPTIONS names
      (protocol Appendix B.2); an empty value is the setting's default. They
      reach each worker with its next top-level request. False while the
      session has programs (Oracle's rule). */
  bool set_settings(
      const std::vector<std::pair<std::string, std::string>> &settings);

 private:
  /** A worker of the session: one per principal and worker kind. */
  struct Slot {
    std::unique_ptr<mle::Worker> worker;
    std::string principal;
    WorkerKind kind = WorkerKind::kJvm;
    /** The options in force in the worker, and what they were made from:
        the session's zone as the server names it and the version of the
        session's settings. */
    mle::OptionsMsg options;
    std::string zone;
    uint64_t settings_version = 0;
    /** engine_forgets() when no account of the principal was known to be
        dropped or renamed. */
    uint64_t forgets = 0;
    /** Routines that are gone but whose DROP could not be sent yet. */
    std::vector<uint64_t> undropped;
  };
  /** The key of a slot in m_workers: the principal and the kind of the
      language's worker ('\0' between: not in account names). */
  static std::string slot_key(const std::string &principal, unsigned language);
  /** The key as the error log shows it: "'u'@'h' (v8)". */
  static std::string display_key(const std::string &key);

  explicit Session(MYSQL_THD thd);

  /** The principal for the security context in force and, with a routine,
      its definer: the account of the context when both are the same,
      else "<account> running code of <definer>". Code of different owners
      never shares a worker, where it could change the language's shared
      objects (JSON.parse, builtins, statics) under the other's code. */
  std::string principal(const Routine *routine = nullptr) const;
  /** Releases the workers (kept for their principals if reuse) and forgets
      programs, console, stack trace, settings. */
  void clear_state(bool reuse);
  /** The principal's worker, started if needed and ready for a request: at
      top level with the session's options in force. nullptr after reporting
      the error (or for a session that is killed: the server reports it). */
  Slot *worker_for(const std::string &key, const Routine &routine);
  /** OPTIONS when the session's zone or settings differ from the options in
      force in the slot's worker, or none are. False after reporting. */
  bool options_in(const std::string &key, Slot &slot, const Routine &routine);
  /** PREPAREs the routine in the slot's worker unless it is prepared there. */
  bool prepare_in(const std::string &key, Slot &slot, Routine &routine);
  /** The principal's worker with the routine prepared in it; nullptr after
      reporting the error. */
  mle::Worker *prepared_worker(const std::string &key, Routine &routine);
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
      is installed with the first request and interrupts every worker that
      has a top-level request of this session in progress (a chain of calls
      can pass through several workers). */
  class KillGuard;
  static constexpr size_t kMaxRunning = 64;
  std::array<std::atomic<mle::Worker *>, kMaxRunning> m_running{};
  std::atomic<size_t> m_running_n{0};
  bool m_kill_installed = false;
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
  /** The session's language settings that are not at their default, by
      name; and a number that changes with them. */
  std::map<std::string, std::string> m_settings;
  uint64_t m_settings_version = 1;
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
