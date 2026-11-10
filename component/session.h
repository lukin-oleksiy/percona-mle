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
  @file components/mle/component/session.h

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

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "mle/codec.h"
#include "mle/worker.h"
#include "routine.h"

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
  /** Runs the routine for the current principal. False after reporting
      the error. */
  bool execute(Routine &routine);
  /** The routine is gone (ALTER, DROP, cache eviction): workers drop it. */
  void forget(const Routine &routine);

  /** Console output kept for mle_session_state() (P4-W5), last 64K. */
  const std::string &out() const { return m_out; }
  const std::string &err() const { return m_err; }

 private:
  struct Slot {
    std::unique_ptr<mle::Worker> worker;
    std::unordered_set<uint64_t> prepared;
  };

  explicit Session(MYSQL_THD thd) : m_thd(thd) {}

  std::string principal() const;
  /** The principal's worker, started (and given the options) if needed;
      nullptr after reporting the error. */
  Slot *worker_for(const std::string &principal, const Routine &routine);
  /** PREPAREs the routine in the slot's worker unless it has it. */
  bool prepare_in(const std::string &principal, Slot &slot,
                  const Routine &routine);
  /** Reports a failed request; drops the worker if it is gone. */
  void report(const std::string &principal, mle::Status status,
              const mle::ResultMsg &result, const std::string &error,
              const Routine &routine, const std::vector<mle::Value> *args);
  void keep_console(const mle::ResultMsg &result);
  void drop_worker(const std::string &principal);

  MYSQL_THD m_thd;
  std::map<std::string, Slot> m_workers;
  mle::OptionsMsg m_options;
  bool m_options_bound = false;
  std::string m_out;
  std::string m_err;
};

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_SESSION_H
