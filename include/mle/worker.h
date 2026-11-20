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

  The host's handle on one worker process (libmle_host). It runs the host's
  side of the MLE worker protocol (docs/protocol.md; section numbers below
  refer to it): the mailbox and its turns, READY, the requests and their
  RESULTs, the worker's SQL and LIBRARY messages (served through CallHooks),
  interruption with its grace clock, deadlines, and the validation of
  everything the worker sends.

  The worker is untrusted. All protocol state is private to this object; a
  message header is copied out of the region once, a chunk's bytes are
  copied once, and nothing the host wrote is read back. A violation ends
  the worker. A worker that died, was ended or retired is never used again:
  every later request returns Outcome::kDied with failure() as the reason.

  Threads: one thread at a time uses a Worker (the session's thread);
  interrupt() and interrupted() may be called from any thread. Nested use
  from CallHooks::sql on the same thread is expected.
*/

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mle/codec.h"
#include "mle/launcher.h"
#include "mle/protocol.h"

namespace mle {

using wire::Status;

/** How a request ended in the host (§11.3). The first five are the status
    of the worker's RESULT (§9.4) with the same values; the others are the
    host's own. */
enum class Outcome : uint32_t {
  kOk = 0,
  kGuestError = 1,
  kInterrupted = 2,
  kExhausted = 3,
  kInternal = 4,
  /** The worker exited or was killed by a signal; also: the worker was
      retired earlier and cannot be used. */
  kDied,
  /** The host ended the worker after the grace period (§11.1). */
  kKilled,
  /** The worker broke the protocol and was ended. */
  kViolation,
  /** The worker missed a deadline (§11.2) and was ended. */
  kTimedOut,
  /** READY shows a worker of another major version, or one without the
      language that is needed (§9.1); it was retired. */
  kIncompatible,
  /** The host did not send the request: it is not allowed at this point or
      exceeds a limit the host keeps (nesting depth, message size, a routine
      that is not prepared, no options in force). The worker is unaffected. */
  kRefused,
};

const char *outcome_name(Outcome outcome);

struct WorkerOptions {
  /** Poll this long for the worker's post before sleeping on the futex. */
  int spin_us = 50;
  /** The grace period G of §11.1. */
  int interrupt_grace_ms = 1000;
  /** The deadline of DROP, RESET and OPTIONS (§11.2). */
  int control_timeout_ms = 10000;
};

/** The arena, for a reply written straight into it: a ROWS SQL_RESULT's
    bytes, without a SqlResultMsg. The writer sets posts_at_start to *posts
    when it starts writing, and length when it is done; nothing may be
    posted to the worker in between (that would overwrite the arena): the
    Worker checks. Such a reply is one chunk. */
struct DirectReply {
  char *data = nullptr;
  size_t capacity = 0;
  const uint64_t *posts = nullptr;
  uint64_t posts_at_start = 0;
  size_t length = 0;
  /** The rows written (for the stage trace). */
  uint32_t rows = 0;
};

/** Why the Worker answers an SQL message itself (CallHooks::sql_refused). */
enum class SqlRefusal {
  /** An interrupt was noticed (§11.1 step 2). */
  kInterrupted,
  /** The worker is serving PREPARE (§8.1). */
  kPreparing,
  /** The message would put the session over its bound on request bytes;
      it was discarded and only its action and statement id are known
      (§5.3). */
  kRequestTooLarge,
  /** A parameter is a float that is not finite (§7.2). */
  kNonFiniteParameter,
  /** The statement ran, but its SQL_RESULT would exceed the limit of a
      host message (§5.3). */
  kReplyTooLarge,
};

/** Implemented by the caller (mysqld in the component, tests here). */
class CallHooks {
 public:
  virtual ~CallHooks() = default;
  /** Runs an SQL message of the routine on the calling thread and fills
      *result (§10). May use the Worker again for nested requests (PREPARE,
      CALL, DROP). The Worker sets error_id of an ERROR itself. The time
      spent in this call is not counted on the grace clock (§11.1 step 3,
      exception (a)), except while a nested request of this worker is in
      progress. */
  virtual void sql(const SqlMsg &request, SqlResultMsg *result) = 0;
  /** sql(), but a ROWS result may instead be written into direct (its
      length set). By default: sql(). */
  virtual void sql_into(const SqlMsg &request, SqlResultMsg *result,
                        DirectReply *direct) {
    (void)direct;
    sql(request, result);
  }
  /** The Worker answers an SQL message without running it, or replaces the
      reply of one that ran (kReplyTooLarge, and kInterrupted when the
      interrupt was noticed after sql() returned). The hook applies §10.1 to
      the statement id: CLOSE closes it; any other action is answered ERROR
      by rule 2 or 3 (an allowed action discards the id's results and sends
      the id home). It fills the ERROR's number, SQLSTATE and message in
      *error; the Worker has set kind and failed_row, and sets error_id. For
      CLOSE the Worker answers OK and ignores *error. The hook executes
      nothing and does not use the Worker. By default: a generic error. */
  virtual void sql_refused(SqlRefusal why, wire::SqlAction action,
                           uint64_t stmt_id, SqlResultMsg *error);
  /** Asks to hold an SQL message of bytes bytes while its statement is in
      progress (§10.7). False: the session's bound would be exceeded; the
      Worker discards the message and answers ERROR (kRequestTooLarge).
      Every accepted reservation is released once the reply is posted or the
      request fails. By default there is no bound. */
  virtual bool sql_bytes_reserve(uint64_t bytes) {
    (void)bytes;
    return true;
  }
  virtual void sql_bytes_release(uint64_t bytes) { (void)bytes; }
  /** True once the session was killed or its statement ran out of time;
      polled at every bounded sleep and after every message of the worker. */
  virtual bool killed() { return false; }
  /** A database library the routine imports (§9.10); by default REFUSED.
      Not called once an interrupt was noticed. Must not use the Worker. */
  virtual void library(const LibraryMsg &request, LibraryResultMsg *result) {
    (void)request;
    result->status = wire::LibraryStatus::kRefused;
    result->message = "libraries are not available here";
  }
};

/** CallHooks that run no SQL: every action but CLOSE gets an ERROR. */
class NoSql : public CallHooks {
 public:
  void sql(const SqlMsg &request, SqlResultMsg *result) override;
};

/** Worker statistics, with impossible values replaced (§3.1). */
struct WorkerStats {
  uint64_t heap_used = 0;
  uint64_t heap_max = 0;
  uint64_t memory_gcs = 0;
  uint32_t state = 0;
  bool tainted = false;
};

/** An SQL_RESULT ERROR the host sent during the current top-level request
    and still holds (§10.8). */
struct HeldError {
  uint64_t error_id = 0;
  uint32_t sql_errno = 0;
  std::string sqlstate;
  std::string message;
};

class Worker {
 public:
  explicit Worker(std::unique_ptr<WorkerProcess> process,
                  const WorkerOptions &options = {});
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  /** New values of the dynamic settings; ignored under a running request. */
  void set_timing(int spin_us, int interrupt_grace_ms) {
    if (m_depth != 0) return;
    m_options.spin_us = spin_us;
    m_options.interrupt_grace_ms = interrupt_grace_ms;
  }

  /** Waits for READY (§9.1) until timeout_ms from now. kOk; kInternal (the
      worker reported that it cannot serve; *error is its info);
      kIncompatible (another major version, or language is not empty and
      not among the worker's); kViolation, kDied, kTimedOut; kInterrupted
      when hooks->killed() became true first. After anything but kOk the
      worker is ended. */
  Outcome wait_ready(int timeout_ms, ReadyMsg *ready, std::string *error,
                     CallHooks *hooks = nullptr, std::string_view language = {});

  /** PREPARE (§9.2). The RESULT goes to *result; *error says why for every
      outcome but kOk. After anything but kOk the id is not prepared. */
  Outcome prepare(const PrepareMsg &routine, CallHooks &hooks,
                  ResultMsg *result, std::string *error);

  /** CALL (§9.3) of a prepared routine; library_epoch is the host's epoch
      now (§9.10). A return or OUT value that is not finite gives
      kGuestError with the language wire::kLanguageType, as §9.4 says. */
  Outcome call(uint64_t routine_id, uint64_t library_epoch,
               const std::vector<Value> &args, CallHooks &hooks,
               ResultMsg *result, std::string *error);

  /** DROP (§9.6), top level or nested. */
  Outcome drop(uint64_t routine_id, std::string *error);
  /** OPTIONS (§9.8). With kGuestError *guest_error is the worker's Error:
      its language (wire::kLanguageTimeZone or kLanguageOption) tells which
      setting it refused. */
  Outcome options(const OptionsMsg &options, Error *guest_error,
                  std::string *error);
  /** RESET (§9.7). Ends the options in force. */
  Outcome reset(std::string *error);
  /** PARSE_LIBRARY (§9.9). */
  Outcome parse_library(const ParseLibraryMsg &library, CallHooks &hooks,
                        ResultMsg *result, std::string *error);

  /** Requests an interrupt of the interruptible top-level request in
      progress (§11.1 step 1): records it, then sends INTERRUPT. Any thread.
      Without such a request it does nothing; CallHooks::killed covers a
      request that has not started. */
  void interrupt();
  /** True if an interrupt was requested for the top-level request in
      progress or, between requests, for the last one: the client is then
      told "interrupted" whatever the outcome (§11.1 step 4). A request that
      was not sent (kRefused, a worker that is gone, a session killed before
      it started) has no record: false after it. */
  bool interrupted() const { return m_recorded.load(); }

  /** The ERROR with this error_id, if it was sent during the top-level
      request in progress (or the last one) and is still held; else nullptr.
      Valid until the next request. */
  const HeldError *held_error(uint64_t error_id) const;

  /** True while OPTIONS answered OK and no RESET followed (§9.8). */
  bool options_in_force() const { return m_options_in_force; }
  /** True while the id is prepared in the worker (§8.3). */
  bool is_prepared(uint64_t routine_id) const {
    return m_routines.contains(routine_id);
  }
  /** The READY languages. */
  const std::vector<std::string> &languages() const { return m_languages; }
  /** The minor version in use with this worker (§14). */
  uint32_t minor_version() const { return m_minor; }

  /** False once the worker is dead, ended or retired. */
  bool usable() const { return m_usable; }
  /** Why the worker is no longer usable. */
  const std::string &failure() const { return m_failure; }

  WorkerStats stats() const;
  WorkerProcess &process() { return *m_process; }
  /** Requests in progress: 0 when idle. */
  uint32_t depth() const { return m_depth; }

 private:
  struct SqlTrace;
  struct Message {
    wire::MessageHeader h{};
    std::string body;
    /** An SQL message over the bound on request bytes: body holds only its
        action and statement id. */
    bool discarded = false;
    /** Bytes reserved with CallHooks::sql_bytes_reserve. */
    uint64_t reserved = 0;
  };
  enum class Wait { kMessage, kDied, kKilled, kTimeout, kProtocol };

  struct RoutineInfo {
    size_t params = 0;
    bool has_return = false;
    /** Indexes of the OUT and INOUT parameters, ascending. */
    std::vector<uint32_t> outs;
  };

  /** A message in chunks, as the grace clock sees it (§11.1 step 3 (b)). */
  struct Transfer {
    uint64_t total_len = 0;
    /** Its turns are bounded on their own and not counted on the clock. */
    bool excepted = false;
    /** The worker's turns of it, counted so far. */
    int64_t used_ns = 0;
  };

  /** What the host expects of the next message of the worker. */
  struct Expect {
    /** Bit per allowed op. */
    uint32_t ops = 0;
    uint32_t depth = 0;
    uint64_t call_seq = 0;
    /** The request being served (READY while waiting for it). */
    wire::Op request = wire::Op::kReady;
  };

  struct Request {
    wire::Op op;
    std::string_view body;
    uint64_t routine_id = 0;
    const RoutineInfo *routine = nullptr;
    /** PREPARE with check. */
    bool check = false;
  };

  Outcome request(const Request &q, CallHooks &hooks, ResultMsg *result,
                  std::string *error);
  /** The checks of §9.4 and of the request table of §9 on a decoded
      RESULT. Empty if it obeys them, else what it breaks. */
  std::string check_result(const Request &q, uint32_t status,
                           const ResultMsg &r) const;
  /** Serves an SQL or a LIBRARY message of the worker: posts the reply. */
  Wait serve_sql(const Request &q, Message &m, CallHooks &hooks,
                 int64_t deadline, std::string *error);
  Wait serve_library(Message &m, CallHooks &hooks, int64_t deadline,
                     std::string *error);

  /** Sends body as one message, in chunks if needed. begins_top: the
      first chunk begins a top-level request (it takes the next call_seq). */
  Wait send(wire::Op op, std::string_view body, CallHooks *hooks,
            bool begins_top, bool interruptible, int64_t deadline,
            std::string *error);
  /** Waits for and reassembles the next message. */
  Wait receive(Message *m, CallHooks *hooks, const Expect &expect,
               int64_t deadline, std::string *error);
  /** Waits for the worker's post. transfer: the transfer this turn of the
      worker belongs to, or nullptr; turn_len: the chunk the worker reads
      or posts in this turn, 0 if the turn has no bound of its own. */
  Wait wait_post(CallHooks *hooks, Transfer *transfer, uint64_t turn_len,
                 int64_t deadline, std::string *error);
  /** Looks for the interrupt record and keeps the grace clock (§11.1 step
      3). transfer: the transfer in progress, or nullptr; worker_has_turn:
      the worker has not posted since the host's last post. kKilled when a
      bound is exceeded. */
  Wait tick(Transfer *transfer, bool worker_has_turn, uint64_t turn_len,
            std::string *error);
  /** The worker posted: closes its turn inside an excepted transfer. */
  Wait end_turn(Transfer *transfer, uint64_t turn_len, std::string *error);
  /** The bounds of a turn inside an excepted transfer, at time now. */
  Wait check_turn(const Transfer &transfer, uint64_t turn_len, int64_t now,
                  std::string *error) const;
  /** The grace clock counts from now, or stops; without effect before the
      moment of notice. */
  void clock_start(int64_t now);
  void clock_stop(int64_t now);
  void post(wire::Op op, uint32_t flags, const char *data, size_t len,
            uint64_t total);
  /** Ends the worker and makes it unusable; returns outcome. */
  Outcome fail(Outcome outcome, const std::string &why, std::string *error);
  Outcome died(std::string *error);
  Outcome lost(Wait w, wire::Op op, const std::string &why, std::string *error);
  void hold_error(SqlResultMsg *error);
  /** A request is asked for: at top level, the record of the last
      top-level request is not this one's. */
  void forget_record();

  std::unique_ptr<WorkerProcess> m_process;
  WorkerOptions m_options;
  bool m_ready = false;
  bool m_usable = true;
  std::string m_failure;
  std::vector<std::string> m_languages;
  uint32_t m_minor = 0;
  bool m_options_in_force = false;

  // The mailbox (§4): private counts, never read back from the region.
  /** Posts made: the value of the host's sequence word. */
  uint32_t m_posted = 0;
  /** The worker's sequence word as last seen. */
  uint32_t m_seen = 0;
  /** Messages posted (DirectReply's check). */
  uint64_t m_posts = 0;
  /** The process was found ended and its last post is being read (§11.4). */
  bool m_dead = false;

  // The dialogue (§8).
  uint32_t m_depth = 0;
  /** Requests whose CallHooks::sql is running: a nested request is allowed
      only when this equals m_depth. */
  uint32_t m_sql_hooks = 0;
  /** call_seq of the last top-level request sent. */
  uint64_t m_seq = 0;
  std::unordered_map<uint64_t, RoutineInfo> m_routines;
  /** Ids whose nested PREPARE or DROP was not sent after an interrupt: a
      top-level DROP goes out before the worker is used again (§11.1 step 2). */
  std::unordered_set<uint64_t> m_unsent_drops;
  /** A nested DROP answered INTERNAL: retire after the top-level request. */
  bool m_retire_after_top = false;
  std::string m_retire_why;

  // Interruption (§11.1). The mutex serializes the record, the post of a
  // top-level request's first chunk and the receipt of its RESULT.
  mutable std::mutex m_interrupt_mutex;
  /** call_seq of the interruptible top-level request in progress, or 0. */
  uint64_t m_running = 0;
  /** "Interrupt requested" for the top-level request in progress or last. */
  std::atomic<bool> m_recorded{false};

  // The grace clock, on the session's thread.
  /** The hooks of the interruptible top-level request in progress: its
      killed() is what every wait of the request polls. */
  CallHooks *m_kill_hooks = nullptr;
  /** The session's thread has found the record: the moment of notice. */
  bool m_noticed = false;
  bool m_clock_running = false;
  int64_t m_clock_mark = 0;
  int64_t m_clock_used = 0;
  int64_t m_clock_limit = 0;
  /** Start of the worker's current turn inside an excepted transfer. */
  int64_t m_turn_start = 0;
  /** The host message being sent or sent last, if it has chunks. */
  Transfer m_host_transfer;
  bool m_host_transfer_open = false;
  /** total_len of the host message posted last. */
  uint64_t m_last_host_total = 0;

  // ERRORs held for §10.8.
  std::array<HeldError, wire::kHeldErrors> m_held;
  uint64_t m_error_ids = 0;

  SqlTrace *m_trace = nullptr;
  /** Body of the top-level CALL, reused. */
  std::string m_call_body;
};

}  // namespace mle

#endif  // MLE_WORKER_H
