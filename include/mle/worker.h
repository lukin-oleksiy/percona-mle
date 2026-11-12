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

#ifndef MLE_WORKER_H
#define MLE_WORKER_H

/**
  @file include/mle/worker.h

  The host's handle on one worker process (libmle_host): runs the mailbox
  protocol (protocol.h) for READY, PREPARE, CALL, DROP and RESET, serves
  the worker's SQL requests through CallHooks, and stops or kills the
  worker on KILL, death or misbehaviour.

  The worker is untrusted: every message is copied out of shared memory
  and validated (header, expected op, depth, call sequence, body, status
  and error consistency). A violation kills the worker. A worker that died,
  was killed, broke the protocol or reported itself tainted is never used
  again: every later request returns Status::kDied.

  Threads: one thread at a time uses a Worker (the session's thread);
  interrupt() may be called from any thread (e.g. the KILL handler).
  Nested use from CallHooks::sql on the same thread is expected.
*/

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mle/codec.h"
#include "mle/launcher.h"
#include "mle/protocol.h"

namespace mle {

using wire::Status;

struct WorkerOptions {
  /** Spin this long waiting for the worker before sleeping on the futex. */
  int spin_us = 50;
  /** Grace between INTERRUPT and SIGKILL. */
  int interrupt_grace_ms = 1000;
  /** Nested calls (routine -> SQL -> routine ...). */
  uint32_t max_depth = wire::kDefaultMaxDepth;
  /** Largest message accepted from the worker. */
  uint64_t max_message = 64ULL << 20;
  /** DROP, OPTIONS and RESET run no routine code: a worker that does not
      answer them in this time is killed (they are not KILLable). */
  int control_timeout_ms = 10000;
};

/** Implemented by the caller (mysqld in the component, tests here). */
class CallHooks {
 public:
  virtual ~CallHooks() = default;
  /** Runs a statement for the routine, on the calling thread, and fills
   *result. May call Worker::call again (a nested routine call). */
  virtual void sql(const SqlMsg &request, SqlResultMsg *result) = 0;
  /** True once the session was killed or ran out of time; polled while
      waiting (every few milliseconds) and after each SQL request. */
  virtual bool killed() { return false; }
  /** A database library the routine imports (Phase 6); by default none. */
  virtual void library(const LibraryMsg &request, LibraryResultMsg *result) {
    (void)request;
    result->status = wire::LibraryStatus::kRefused;
    result->message = "libraries are not available here";
  }
};

/** CallHooks for requests that must not run SQL (PREPARE, DROP, RESET). */
class NoSql : public CallHooks {
 public:
  void sql(const SqlMsg &, SqlResultMsg *result) override;
};

/** Worker statistics, clamped (the worker writes them). */
struct WorkerStats {
  uint64_t heap_used = 0;
  uint64_t heap_max = 0;
  uint32_t state = 0;
  bool tainted = false;
};

class Worker {
 public:
  explicit Worker(std::unique_ptr<WorkerProcess> process,
                  const WorkerOptions &options = {});
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  /** mle.spin_us and mle.interrupt_grace_ms are dynamic: a kept or long
      used worker takes their values at each top-level call. */
  void set_timing(int spin_us, int interrupt_grace_ms) {
    if (m_depth != 0) return;  // not under a running call
    m_options.spin_us = spin_us;
    m_options.interrupt_grace_ms = interrupt_grace_ms;
  }

  /** Waits for READY. kOk, or kInternal (the worker reported a start
      failure), kDied, kProtocolError, with *error. */
  Status wait_ready(int timeout_ms, ReadyMsg *ready, std::string *error,
                    CallHooks *hooks = nullptr);

  /** Compiles a routine. The RESULT goes to *result (with the compile
      error for kGuestError); host-side failures (kDied, kKilled,
      kProtocolError, kInterrupted before the worker answered) are in
      *error. */
  Status prepare(const PrepareMsg &routine, CallHooks &hooks, ResultMsg *result,
                 std::string *error);

  /** Calls a prepared routine. While waiting, serves SQL requests with
      hooks.sql and polls hooks.killed(). */
  Status call(uint64_t routine_id, uint32_t events,
              const std::vector<Value> &args, CallHooks &hooks,
              ResultMsg *result, std::string *error);

  Status drop(uint64_t routine_id, std::string *error);
  /** Conversion options and time zone of the session (OPTIONS). A zone
      the worker does not know gives kGuestError, *error says why. */
  Status options(const OptionsMsg &options, std::string *error);
  /** mle_session_reset(): the worker forgets all routines and state. */
  Status reset(std::string *error);
  /** CREATE LIBRARY: syntax check of a library body (nothing runs). A
      syntax error is kGuestError with result->error. */
  Status parse_library(const ParseLibraryMsg &library, CallHooks &hooks,
                       ResultMsg *result, std::string *error);

  /** Interrupts the running top-level call (any thread). Without a running
      call it does nothing; CallHooks::killed covers the next call. */
  void interrupt();

  /** False once the worker is dead, killed, retired or misbehaved. */
  bool usable() const { return m_usable; }
  /** Why the worker is no longer usable. */
  const std::string &failure() const { return m_failure; }

  WorkerStats stats() const;
  WorkerProcess &process() { return *m_process; }
  /** Current nesting depth (0 when idle). */
  uint32_t depth() const { return m_depth; }

 private:
  struct Message {
    wire::MessageHeader h{};
    std::string body;
  };
  enum class Wait { kMessage, kDied, kKilled, kTimeout, kProtocol };

  struct RoutineInfo {
    size_t params;
    bool has_return;
    /** Indexes of the OUT and INOUT parameters. */
    std::vector<uint32_t> outs;
  };

  /** timeout_ms bounds each wait for the worker (-1: none, for requests
      that run routine code and end on KILL instead). */
  Status request(wire::Op op, const std::string &body, CallHooks &hooks,
                 const RoutineInfo *routine, ResultMsg *result,
                 std::string *error, int timeout_ms = -1);
  /** Sends body as one message (chunked if needed). */
  Wait send(wire::Op op, const std::string &body, CallHooks *hooks,
            std::string *error, int timeout_ms = -1);
  /** Waits for and reassembles the next message. */
  Wait receive(Message *m, CallHooks *hooks, int timeout_ms,
               std::string *error);
  Wait wait_change(CallHooks *hooks, int timeout_ms, std::string *error);
  /** KILL handling: INTERRUPT once, SIGKILL after the grace. kKilled when the
      worker was killed, else kMessage. */
  Wait check_kill(CallHooks *hooks);
  void post(wire::Op op, uint32_t flags, const char *data, size_t len,
            uint64_t total);
  /** Kills the worker and makes it unusable; returns status. */
  Status fail(Status status, const std::string &why, std::string *error);
  Status died(std::string *error);

  std::unique_ptr<WorkerProcess> m_process;
  WorkerOptions m_options;
  bool m_ready = false;
  bool m_usable = true;
  std::string m_failure;
  uint32_t m_seen_h = 0;
  uint32_t m_depth = 0;
  uint64_t m_seq = 0;
  /** call_seq of the running top-level request, 0 when idle. */
  std::atomic<uint64_t> m_running{0};
  /** When the INTERRUPT went out for the running request (ms clock). */
  int64_t m_interrupted_at_ms = 0;
  bool m_killed_by_us = false;
  std::unordered_map<uint64_t, RoutineInfo> m_routines;
  std::string m_buf;
  /** Body of the top-level CALL, reused. */
  std::string m_call_body;
};

}  // namespace mle

#endif  // MLE_WORKER_H
