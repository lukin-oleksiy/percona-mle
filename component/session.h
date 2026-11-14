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

  The MLE state of a client session (thd_store slot): one worker per
  principal (D-8: the definer for SQL SECURITY DEFINER routines, else the
  current user, as priv_user@priv_host), the routines each worker has
  prepared, the conversion options and time zone (bound at the session's
  first use, as Oracle), and the console output. Created at the session's
  first MLE use, freed with the session; its workers are killed then.

  Only the session's own thread uses it (deinit gets the THD explicitly).
  UNINSTALL is refused while sessions exist: the server does not free
  objects of an unregistered thd_store slot.
*/

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "mle/codec.h"
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
      the current principal compiles the routine (syntax errors -> 6113). */
  bool parse(Routine &routine);
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
      programs, stack trace, options and time-zone binding forgotten. False
      while a routine of this session runs (it would destroy its worker). */
  bool reset();
  /** COM_CHANGE_USER: the session's state belongs to the old account; its
      workers go back to the engine for that account's later sessions. */
  void account_changed();
  /** Programs prepared in this session's workers (Oracle: "active"). */
  bool has_programs() const { return !m_programs.empty(); }
  /** The pid of the current principal's worker, or -1 (debug UDF). */
  long worker_pid();
  /** mle_set_session_state(): sent to the session's workers at once. */
  bool set_options(mle::wire::IntegerType integer_type,
                   mle::wire::DecimalType decimal_type);

 private:
  struct Slot {
    std::unique_ptr<mle::Worker> worker;
    std::unordered_set<uint64_t> prepared;
  };

  explicit Session(MYSQL_THD thd);

  /** The account the current request runs as ('user'@'host', from the
      security context's contents on every call). */
  std::string principal() const;
  /** Releases the workers (kept for their principals if reuse) and forgets
      programs, console, stack trace, options. */
  void clear_state(bool reuse);
  /** The principal's worker, started (and given the options) if needed;
      nullptr after reporting the error. */
  Slot *worker_for(const std::string &principal, const Routine &routine);
  /** PREPAREs the routine in the slot's worker unless it has it. */
  bool prepare_in(const std::string &principal, Slot &slot,
                  const Routine &routine);
  /** Reports a failed request; drops the worker if it is gone. */
  void report(const std::string &principal, mle::Status status,
              const mle::ResultMsg &result, const std::string &error,
              const Routine &routine, const std::vector<mle::Value> *args,
              SqlRunner *sql = nullptr);
  void keep_console(const mle::ResultMsg &result);
  void keep_stack_trace(const mle::Error &error, const Routine &routine);
  void drop_worker(const std::string &principal);
  /** KILL handler on the session while requests run. The server keeps one
      handler per session and refuses to replace it, so it is installed by
      the outermost request and interrupts every worker on the stack
      (nested calls: routine -> SQL -> routine). */
  class KillGuard;
  static constexpr size_t kMaxNesting = 64;
  std::array<std::atomic<mle::Worker *>, kMaxNesting> m_running{};
  std::atomic<size_t> m_running_n{0};
  /** KILLs seen by the kill handler (a call killed meanwhile is interrupted
      even if the KILL was consumed by an inner statement). */
  std::atomic<uint64_t> m_kills{0};
  bool m_kill_installed = false;
  /** Requests of this session in progress (nested: routine -> SQL ->
      routine). A worker dropped by a nested request is kept here until the
      outermost one ends: outer requests still run in it. */
  size_t m_calls = 0;
  std::vector<std::unique_ptr<mle::Worker>> m_retired;

  MYSQL_THD m_thd;
  /** engine_session_begin(); renewed at COM_CHANGE_USER. */
  uint64_t m_mark;
  std::map<std::string, Slot> m_workers;
  /** Argument buffer of the top-level call, reused. */
  std::vector<mle::Value> m_args;
  mle::OptionsMsg m_options;
  bool m_options_bound = false;
  std::string m_out;
  std::string m_err;
  /** Oracle's stack_trace: the frames of the last call's runtime error. */
  std::string m_stack;
  /** Routine id -> its type (stored_functions / stored_procedures). */
  std::map<uint64_t, uint16_t> m_programs;
};

/** CREATE FUNCTION/PROCEDURE needs CREATE_<LANGUAGE>_ROUTINE too (D-3);
    false after reporting the missing privilege. Other statements pass. */
bool check_create_privilege(MYSQL_THD thd, const Routine &routine);

/** CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE. */
bool register_privileges();
void unregister_privileges();

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_SESSION_H
