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

#include "mle/worker.h"

#include <linux/futex.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace mle {

/** The stages of one top-level CALL's SQL, in nanoseconds: printed as one
    line on stderr (the error log) when MLE_SQL_TRACE is set in mysqld's
    environment. */
struct Worker::SqlTrace {
  int64_t worker = 0;  // waiting for the worker (it decodes, runs guest code)
  int64_t sql = 0;     // the SQL hook: execute, read rows into Values
  int64_t exec = 0;    // of which the server's execution (exec_us)
  int64_t encode = 0;  // Values into the reply buffer
  int64_t send = 0;    // the reply into the arena (and its chunks)
  uint64_t requests = 0, rows = 0, bytes = 0;
  void print(int64_t total) const {
    fprintf(stderr,
            "MLE SQL trace: requests %llu rows %llu bytes %llu | worker %.3f "
            "sql %.3f (execute %.3f) encode %.3f send %.3f | total %.3f ms\n",
            (unsigned long long)requests, (unsigned long long)rows,
            (unsigned long long)bytes, worker / 1e6, sql / 1e6, exec / 1e6,
            encode / 1e6, send / 1e6, total / 1e6);
  }
};

namespace {

/** The host's bounded sleep: how often kills, deadlines and the worker's
    death are looked for while the worker is silent (§4.3). */
constexpr int kTickMs = 10;
constexpr int kExitReportMs = 1000;

using wire::Op;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool sql_trace_enabled() {
  static const bool on = [] {
    const char *v = getenv("MLE_SQL_TRACE");
    return v != nullptr && *v != '\0' && strcmp(v, "0") != 0;
  }();
  return on;
}

void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// The words are shared between processes: no FUTEX_PRIVATE_FLAG.
void futex_wait(std::atomic<uint32_t> *word, uint32_t value, int timeout_ms) {
  timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
  syscall(SYS_futex, reinterpret_cast<uint32_t *>(word), FUTEX_WAIT, value, &ts,
          nullptr, 0);
}

void futex_wake(std::atomic<uint32_t> *word) {
  syscall(SYS_futex, reinterpret_cast<uint32_t *>(word), FUTEX_WAKE, 1, nullptr,
          nullptr, 0);
}

const char *op_name(uint32_t op) {
  switch (static_cast<Op>(op)) {
    case Op::kReady:
      return "READY";
    case Op::kPrepare:
      return "PREPARE";
    case Op::kCall:
      return "CALL";
    case Op::kResult:
      return "RESULT";
    case Op::kSql:
      return "SQL";
    case Op::kSqlResult:
      return "SQL_RESULT";
    case Op::kDrop:
      return "DROP";
    case Op::kReset:
      return "RESET";
    case Op::kContinue:
      return "CONTINUE";
    case Op::kOptions:
      return "OPTIONS";
    case Op::kParseLibrary:
      return "PARSE_LIBRARY";
    case Op::kLibrary:
      return "LIBRARY";
    case Op::kLibraryResult:
      return "LIBRARY_RESULT";
  }
  return "an unassigned op";
}
const char *op_name(Op op) { return op_name(static_cast<uint32_t>(op)); }

constexpr uint32_t bit(Op op) { return 1U << static_cast<uint32_t>(op); }

/** PREPARE, CALL and PARSE_LIBRARY end by RESULT or by interruption; DROP,
    RESET and OPTIONS have a deadline and one-chunk messages. */
constexpr bool is_interruptible(Op op) {
  return op == Op::kPrepare || op == Op::kCall || op == Op::kParseLibrary;
}

/** grace x bytes / 16 MiB, exact, rounded up to the nanosecond. */
int64_t scaled(int64_t grace_ns, uint64_t bytes) {
  const auto product = static_cast<unsigned __int128>(grace_ns) * bytes;
  return static_cast<int64_t>((product + wire::kGraceBytes - 1) /
                              wire::kGraceBytes);
}

std::string ms_text(int64_t ns) { return std::to_string(ns / 1000000) + " ms"; }

/** Gives back, at the end of a message's handling, the request bytes that
    were reserved for it. */
class Reservation {
 public:
  Reservation(CallHooks *hooks, const uint64_t *bytes)
      : m_hooks(hooks), m_bytes(bytes) {}
  ~Reservation() {
    if (*m_bytes != 0) m_hooks->sql_bytes_release(*m_bytes);
  }
  Reservation(const Reservation &) = delete;
  Reservation &operator=(const Reservation &) = delete;

 private:
  CallHooks *m_hooks;
  const uint64_t *m_bytes;
};

}  // namespace

const char *outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::kOk:
      return "OK";
    case Outcome::kGuestError:
      return "GUEST_ERROR";
    case Outcome::kInterrupted:
      return "INTERRUPTED";
    case Outcome::kExhausted:
      return "EXHAUSTED";
    case Outcome::kInternal:
      return "INTERNAL";
    case Outcome::kDied:
      return "died";
    case Outcome::kKilled:
      return "killed";
    case Outcome::kViolation:
      return "violation";
    case Outcome::kTimedOut:
      return "timed out";
    case Outcome::kIncompatible:
      return "incompatible";
    case Outcome::kRefused:
      return "refused";
  }
  return "?";
}

void CallHooks::sql_refused(SqlRefusal why, bool, wire::SqlAction, uint64_t,
                            uint64_t, SqlResultMsg *error) {
  // MySQL's ER_QUERY_INTERRUPTED and ER_UNKNOWN_ERROR.
  error->sql_errno = why == SqlRefusal::kInterrupted ? 1317 : 1105;
  error->sqlstate = why == SqlRefusal::kInterrupted ? "70100" : "HY000";
  switch (why) {
    case SqlRefusal::kInterrupted:
      error->message = "Query execution was interrupted";
      break;
    case SqlRefusal::kPreparing:
      error->message = "SQL is not allowed while a routine is being loaded";
      break;
    case SqlRefusal::kRequestTooLarge:
      error->message =
          "The statement and its parameters are larger than MLE allows";
      break;
    case SqlRefusal::kNonFiniteParameter:
      error->message = "A parameter is not a finite number";
      break;
    case SqlRefusal::kReplyTooLarge:
      error->message = "The result is larger than MLE allows";
      break;
  }
}

void NoSql::sql(const SqlMsg &request, SqlResultMsg *result) {
  if (request.action == wire::SqlAction::kClose) {
    result->kind = SqlResultKind::kOk;
    return;
  }
  result->kind = SqlResultKind::kError;
  result->sql_errno = 1105;
  result->sqlstate = "HY000";
  result->message = "SQL is not allowed here";
  result->failed_row =
      request.action == wire::SqlAction::kExecuteMany ? 0 : wire::kNoRow;
}

Worker::Worker(std::unique_ptr<WorkerProcess> process,
               const WorkerOptions &options)
    : m_process(std::move(process)), m_options(options) {}

Worker::~Worker() = default;  // the WorkerProcess ends the worker

// ---------------------------------------------------------------- failures

Outcome Worker::fail(Outcome outcome, const std::string &why,
                     std::string *error) {
  m_process->kill();
  m_usable = false;
  m_failure = why;
  if (error != nullptr) *error = why;
  return outcome;
}

Outcome Worker::died(std::string *error) {
  if (m_usable) {
    m_usable = false;
    m_failure = "the MLE worker died: " +
                m_process->exit_info(kExitReportMs).describe();
  }
  if (error != nullptr) *error = m_failure;
  return Outcome::kDied;
}

Outcome Worker::lost(Wait w, Op op, const std::string &why,
                     std::string *error) {
  switch (w) {
    case Wait::kDied:
      // Some runtimes exit when they are interrupted: a worker that ends
      // after an interrupt was requested was ended by it (§11.4).
      if (m_usable && interrupted()) {
        died(nullptr);
        return fail(Outcome::kKilled,
                    "the MLE worker ended after the interrupt (" +
                        m_process->exit_info(kExitReportMs).describe() + ")",
                    error);
      }
      return died(error);
    case Wait::kKilled:
      return fail(Outcome::kKilled, why, error);
    case Wait::kTimeout:
      return fail(Outcome::kTimedOut,
                  std::string("the MLE worker did not answer ") + op_name(op) +
                      " within " + std::to_string(m_options.control_timeout_ms) +
                      " ms and was terminated",
                  error);
    case Wait::kProtocol:
    case Wait::kMessage:
      break;
  }
  return fail(Outcome::kViolation, "the MLE worker broke the protocol: " + why,
              error);
}

WorkerStats Worker::stats() const {
  // One copy: the worker may change the region between two reads.
  wire::WorkerStats s;
  memcpy(&s, &m_process->header()->stats, sizeof s);
  WorkerStats r;
  r.heap_max = s.heap_max;
  // heap_max 0: the runtime knows no limit (§3.1).
  r.heap_used = s.heap_max == 0 ? s.heap_used : std::min(s.heap_used, s.heap_max);
  r.memory_gcs = s.gc_count <= (uint64_t{1} << 32) ? s.gc_count : 0;
  r.state = s.state <= uint32_t(wire::WorkerState::kCompiling) ? s.state : 0;
  return r;
}

const HeldError *Worker::held_error(uint64_t error_id) const {
  if (error_id == 0) return nullptr;
  // Entries of earlier top-level requests have another call_seq.
  for (const HeldError &e : m_held)
    if (e.error_id == error_id && e.call_seq == m_seq) return &e;
  return nullptr;
}

void Worker::hold_error(const SqlResultMsg &error) {
  HeldError &e = m_held[m_held_next++ % m_held.size()];
  e.call_seq = m_seq;
  e.error_id = error.error_id;
  e.sql_errno = error.sql_errno;
  e.sqlstate = error.sqlstate;  // into the entry's own buffers: reused
  e.message = error.message;
}

std::string &Worker::buffer(
    std::array<std::string, wire::kMaxDepth + 2> &buffers) {
  // A request is refused beyond the deepest nesting, so m_depth fits.
  std::string &b = buffers[std::min<size_t>(m_depth, buffers.size() - 1)];
  // A rare large message does not stay held for the worker's life.
  if (b.capacity() > (4U << 20)) std::string().swap(b);
  b.clear();
  return b;
}

// ---------------------------------------------------------------- mailbox

void Worker::post(Op op, uint32_t flags, const char *data, size_t len,
                  uint64_t total) {
  wire::RegionHeader *h = m_process->header();
  // A DirectReply is already in the arena.
  if (len > 0 && data != reinterpret_cast<const char *>(m_process->arena()))
    memcpy(m_process->arena(), data, len);
  m_posts++;
  wire::MessageHeader m{};
  m.op = static_cast<uint32_t>(op);
  m.flags = flags;
  m.depth = m_depth == 0 ? 0 : m_depth - 1;
  m.len = static_cast<uint32_t>(len);
  m.total_len = total;
  m.call_seq = m_seq;
  memcpy(&h->msg, &m, sizeof m);
  // The post is a store of the private count: the word is the worker's to
  // write too, and is never read back (§4.1).
  h->to_w.store(++m_posted, std::memory_order_seq_cst);
  if (h->w_sleep.load(std::memory_order_seq_cst) != 0) futex_wake(&h->to_w);
  if (m_noticed) m_turn_start = now_ns();
}

void Worker::clock_start(int64_t now) {
  if (!m_noticed || m_clock_running) return;
  m_clock_running = true;
  m_clock_mark = now;
}

void Worker::clock_stop(int64_t now) {
  if (!m_noticed || !m_clock_running) return;
  m_clock_running = false;
  m_clock_used += now - m_clock_mark;
}

Worker::Wait Worker::end_turn(Transfer *transfer, uint64_t turn_len,
                              std::string *error) {
  if (!m_noticed || transfer == nullptr || !transfer->excepted)
    return Wait::kMessage;
  const int64_t now = now_ns();
  const Wait w = check_turn(*transfer, turn_len, now, error);
  transfer->used_ns += now - m_turn_start;
  return w;
}

Worker::Wait Worker::check_turn(const Transfer &transfer, uint64_t turn_len,
                                int64_t now, std::string *error) const {
  const int64_t grace = int64_t{m_options.interrupt_grace_ms} * 1000000;
  const int64_t turn = now - m_turn_start;
  const int64_t turn_limit = grace + scaled(grace, turn_len);
  const int64_t transfer_limit = grace + scaled(grace, transfer.total_len);
  if (turn_len != 0 && turn > turn_limit) {
    *error = "the MLE worker took more than " + ms_text(turn_limit) +
             " for one chunk of a message after the interrupt and was "
             "terminated";
    return Wait::kKilled;
  }
  if (transfer.used_ns + turn > transfer_limit) {
    *error = "the MLE worker took more than " + ms_text(transfer_limit) +
             " for the chunks of a message after the interrupt and was "
             "terminated";
    return Wait::kKilled;
  }
  return Wait::kMessage;
}

Worker::Wait Worker::tick(Transfer *transfer, bool worker_has_turn,
                          uint64_t turn_len, std::string *error) {
  if (m_kill_hooks == nullptr) return Wait::kMessage;
  const int64_t grace = int64_t{m_options.interrupt_grace_ms} * 1000000;
  if (!m_noticed) {
    // The record is made by interrupt(), from any thread; here when the
    // session finds itself killed.
    if (!interrupted() && m_kill_hooks->killed()) interrupt();
    if (!interrupted()) return Wait::kMessage;
    // The moment of notice.
    const int64_t now = now_ns();
    m_noticed = true;
    m_clock_used = 0;
    m_clock_limit = grace;
    m_turn_start = now;
    if (transfer != nullptr) {
      // A transfer in progress is excepted: its turns have their own bounds.
      transfer->excepted = true;
      m_clock_running = false;
    } else {
      m_clock_running = true;
      m_clock_mark = now;
      // The worker may still be taking in the host's last message.
      if (worker_has_turn) m_clock_limit += scaled(grace, m_last_host_total);
    }
    return Wait::kMessage;
  }
  if (transfer != nullptr && transfer->excepted) {
    // The host's turns of an excepted transfer are not counted.
    return worker_has_turn ? check_turn(*transfer, turn_len, now_ns(), error)
                           : Wait::kMessage;
  }
  if (m_clock_running &&
      m_clock_used + now_ns() - m_clock_mark > m_clock_limit) {
    *error = "the MLE worker did not stop within " + ms_text(m_clock_limit) +
             " after the interrupt and was terminated";
    return Wait::kKilled;
  }
  return Wait::kMessage;
}

Worker::Wait Worker::wait_post(CallHooks *hooks, Transfer *transfer,
                               uint64_t turn_len, int64_t deadline,
                               std::string *error) {
  wire::RegionHeader *h = m_process->header();
  // A message has arrived when the worker's word differs from the value
  // seen last; the word is compared only for equality (§4.1).
  auto arrived = [&] {
    const uint32_t v = h->to_h.load(std::memory_order_acquire);
    if (v == m_seen) return false;
    m_seen = v;
    return true;
  };
  // A quick reply needs no clock.
  for (int i = 0; i < 64; i++) {
    if (arrived()) return Wait::kMessage;
    cpu_relax();
  }
  const int64_t spin_until = now_ns() + int64_t{m_options.spin_us} * 1000;
  for (uint32_t i = 0;; i++) {
    if (arrived()) return Wait::kMessage;
    if ((i & 63) == 0 && now_ns() >= spin_until) break;
    cpu_relax();
  }
  // A long wait: what the hooks set up for it (a kill handler) is in place
  // before the kill is looked for and the first sleep.
  if (hooks != nullptr && m_ready) {
    hooks->sleeping();
    if (const Wait w = tick(transfer, true, turn_len, error);
        w != Wait::kMessage)
      return w;
  }
  while (true) {
    // The sleep handshake of §4.3.
    h->h_sleep.store(1, std::memory_order_seq_cst);
    if (h->to_h.load(std::memory_order_seq_cst) == m_seen)
      futex_wait(&h->to_h, m_seen, kTickMs);
    h->h_sleep.store(0, std::memory_order_release);
    if (arrived()) return Wait::kMessage;
    if (!m_process->alive()) {
      // What the worker posted just before it ended is still read (§11.4).
      if (!arrived()) return Wait::kDied;
      m_dead = true;
      return Wait::kMessage;
    }
    // Nothing runs before READY: a kill ends the wait at once.
    if (!m_ready && hooks != nullptr && hooks->killed()) {
      *error = "interrupted";
      return Wait::kKilled;
    }
    if (const Wait w = tick(transfer, true, turn_len, error);
        w != Wait::kMessage)
      return w;
    if (deadline != 0 && now_ns() > deadline) return Wait::kTimeout;
  }
}

Worker::Wait Worker::receive(Message *m, CallHooks *hooks, const Expect &expect,
                             int64_t deadline, std::string *error,
                             bool in_place) {
  const uint32_t arena = m_process->arena_size();
  Assembler a(arena, wire::kMaxWorkerMessage);
  Transfer transfer;
  bool first = true;
  while (true) {
    // The turn in which the worker reads the last chunk of a host message
    // belongs to that message's transfer.
    Transfer *in = first ? (m_host_transfer_open && m_host_transfer.excepted
                                ? &m_host_transfer
                                : nullptr)
                         : &transfer;
    const uint64_t turn_len =
        first ? 0
              : std::min<uint64_t>(arena, transfer.total_len - a.received());
    const Wait w = wait_post(hooks, in, turn_len, deadline, error);
    if (w != Wait::kMessage) return w;
    // Copied once, before any check: the worker can write the region at
    // any time.
    wire::MessageHeader h;
    memcpy(&h, &m_process->header()->msg, sizeof h);
    if (const Wait k = end_turn(in, turn_len, error); k != Wait::kMessage)
      return k;
    if (first) {
      if (in != nullptr) clock_start(now_ns());
      m_host_transfer_open = false;
    }
    // The clock's limit is tested when a post is observed (§11.1 step 3): a
    // RESULT observed after the limit does not save the worker.
    if (m_noticed && m_clock_running &&
        m_clock_used + now_ns() - m_clock_mark > m_clock_limit) {
      *error = "the MLE worker did not stop within " + ms_text(m_clock_limit) +
               " after the interrupt and was terminated";
      return Wait::kKilled;
    }
    const bool more = (h.flags & wire::kMore) != 0;
    const bool result = h.op == static_cast<uint32_t>(Op::kResult);
    if (m_dead &&
        !((result || h.op == static_cast<uint32_t>(Op::kReady)) && !more))
      return Wait::kDied;
    if (!check_worker_header(h, arena, error)) return Wait::kProtocol;
    if (first) {
      // Everything that is checked on a whole message is checked on its
      // first chunk (§5.2).
      if ((expect.ops & (1U << h.op)) == 0) {
        *error = std::string("unexpected ") + op_name(h.op) + " during " +
                 op_name(expect.request);
        return Wait::kProtocol;
      }
      if (h.depth != expect.depth || h.call_seq != expect.call_seq) {
        *error = std::string(op_name(h.op)) + " for the wrong request";
        return Wait::kProtocol;
      }
      const bool small = h.op == static_cast<uint32_t>(Op::kReady) ||
                         h.op == static_cast<uint32_t>(Op::kLibrary) ||
                         (result && !is_interruptible(expect.request));
      if (small && h.total_len > wire::kMaxSmallMessage) {
        *error = std::string(op_name(h.op)) + " larger than its limit";
        return Wait::kProtocol;
      }
    }
    // A message of one chunk is decoded where it is, each byte read once
    // (§1); the chunks of a longer one are copied once as they come.
    const bool leave = first && in_place && !more;
    const Assembler::Step step = a.add(h, m_process->arena(), !leave);
    if (step == Assembler::Step::kError) {
      *error = a.error();
      return Wait::kProtocol;
    }
    if (first && h.op == static_cast<uint32_t>(Op::kSql)) {
      // The bound on request bytes is applied before the message is held
      // or checked (§5.3): only its action and statement id are looked at
      // here, copied once; serve_sql() decodes the rest.
      if (h.total_len < wire::kSqlHeadSize) {
        *error = "bad SQL: message truncated";
        return Wait::kProtocol;
      }
      unsigned char head[wire::kSqlHeadSize];
      memcpy(head,
             leave ? static_cast<const void *>(m_process->arena())
                   : static_cast<const void *>(a.body().data()),
             sizeof head);
      const uint8_t action = head[0];
      m->sql_action = static_cast<wire::SqlAction>(action);
      memcpy(&m->sql_stmt_id, head + 1, sizeof m->sql_stmt_id);
      if (action > wire::kMaxSqlAction) {
        *error = "bad SQL: bad SQL action";
        return Wait::kProtocol;
      }
      if (action == static_cast<uint8_t>(wire::SqlAction::kClose)) {
        if (h.total_len != wire::kSqlCloseSize) {
          *error = "bad SQL: a CLOSE of the wrong length";
          return Wait::kProtocol;
        }
      } else if (hooks->sql_bytes_reserve(h.total_len)) {
        m->reserved = h.total_len;
      } else {
        if (!leave) a.discard_beyond(wire::kSqlHeadSize);
        m->discarded = true;
      }
    }
    if (first && step == Assembler::Step::kNeedMore) {
      // Every transfer of a RESULT is excepted from the grace clock.
      transfer = Transfer{h.total_len, result, 0};
      if (result) clock_stop(m_noticed ? now_ns() : 0);
    }
    // The record is looked for after the post was observed and before the
    // host acts on it or posts anything (§11.1 step 3).
    if (const Wait k = tick(step == Assembler::Step::kNeedMore ? &transfer
                                                                 : nullptr,
                            false, 0, error);
        k != Wait::kMessage)
      return k;
    if (step == Assembler::Step::kDone) {
      if (!first && transfer.excepted) clock_start(m_noticed ? now_ns() : 0);
      m->h = a.header();
      m->in_region = leave;
      if (leave) {
        m->data = std::string_view(
            reinterpret_cast<const char *>(m_process->arena()), h.len);
      } else {
        m->body = a.release_body();
        m->data = m->body;
      }
      return Wait::kMessage;
    }
    post(Op::kContinue, 0, nullptr, 0, 0);
    first = false;
  }
}

Worker::Wait Worker::send(Op op, std::string_view body, CallHooks *hooks,
                          bool begins_top, bool interruptible, int64_t deadline,
                          std::string *error) {
  const uint32_t arena = m_process->arena_size();
  m_host_transfer = Transfer{body.size(), false, 0};
  m_host_transfer_open = body.size() > arena;
  m_last_host_total = body.size();
  size_t sent = 0;
  while (true) {
    const size_t len = std::min<size_t>(arena, body.size() - sent);
    const bool more = sent + len < body.size();
    const uint32_t flags = more ? uint32_t{wire::kMore} : 0U;
    if (begins_top && sent == 0) {
      // The call_seq is used up when the first chunk is posted (§8.2), and
      // an interrupt can be recorded for the request from then on: the word
      // gets kRunning after the post, so a record never names a request
      // that was not sent.
      ++m_seq;
      post(op, flags, body.data(), len, body.size());
      if (interruptible)
        m_interrupt.store(m_seq << 2 | kRunning, std::memory_order_release);
    } else {
      post(op, flags, body.data() + sent, len, body.size());
    }
    sent += len;
    if (!more) return Wait::kMessage;
    const Wait w = wait_post(hooks, &m_host_transfer, len, deadline, error);
    if (w != Wait::kMessage) return w;
    wire::MessageHeader h;
    memcpy(&h, &m_process->header()->msg, sizeof h);
    if (m_dead) return Wait::kDied;
    if (const Wait k = end_turn(&m_host_transfer, len, error);
        k != Wait::kMessage)
      return k;
    if (!check_worker_header(h, arena, error)) return Wait::kProtocol;
    if (h.op != static_cast<uint32_t>(Op::kContinue) ||
        h.depth != (m_depth == 0 ? 0 : m_depth - 1) || h.call_seq != m_seq) {
      *error = std::string("expected CONTINUE for a chunk of ") + op_name(op);
      return Wait::kProtocol;
    }
    if (const Wait k = tick(&m_host_transfer, false, 0, error);
        k != Wait::kMessage)
      return k;
  }
}

// ---------------------------------------------------------------- READY

Outcome Worker::wait_ready(int timeout_ms, ReadyMsg *ready, std::string *error,
                           CallHooks *hooks, std::string_view language) {
  if (!m_usable) return died(error);
  if (m_ready) {
    *error = "the MLE worker is ready already";
    return Outcome::kRefused;
  }
  auto broke = [&](const std::string &what) {
    return fail(Outcome::kViolation,
                "the MLE worker broke the protocol: " + what, error);
  };
  Message m;
  std::string why;
  Expect expect;
  expect.ops = bit(Op::kReady);
  // Copied: a READY's version is read before the rest is decoded.
  switch (receive(&m, hooks, expect,
                  now_ns() + int64_t{timeout_ms} * 1000000, &why, false)) {
    case Wait::kMessage:
      break;
    case Wait::kDied:
      return died(error);
    case Wait::kKilled:
      return fail(Outcome::kInterrupted, "interrupted", error);
    case Wait::kTimeout:
      return fail(Outcome::kTimedOut,
                  "no READY from the MLE worker within " +
                      std::to_string(timeout_ms) + " ms",
                  error);
    case Wait::kProtocol:
      return broke(why);
  }
  auto version_text = [](uint32_t v) {
    return std::to_string(wire::version_major(v)) + "." +
           std::to_string(wire::version_minor(v));
  };
  if (m.h.status != static_cast<uint32_t>(Status::kOk)) {
    // A failed READY: only version and info are decoded (§9.1).
    std::string info;
    if (!decode_ready_head(m.data, &ready->version, &info, &why))
      return broke("bad READY: " + why);
    ready->info = info;
    if (wire::version_major(ready->version) != wire::kVersionMajor)
      return fail(Outcome::kIncompatible,
                  "incompatible MLE worker (protocol version " +
                      version_text(ready->version) + "): " + info,
                  error);
    return fail(Outcome::kInternal, "the MLE worker failed to start: " + info,
                error);
  }
  uint32_t version;
  if (!decode_ready_version(m.data, &version, &why))
    return broke("bad READY: " + why);
  if (wire::version_major(version) != wire::kVersionMajor) {
    ready->version = version;
    return fail(Outcome::kIncompatible,
                "incompatible MLE worker: it speaks protocol version " +
                    version_text(version) + ", the host " +
                    version_text(wire::kVersion),
                error);
  }
  if (!decode(m.data, ready, &why)) return broke("bad READY: " + why);
  if (!language.empty() &&
      std::find(ready->languages.begin(), ready->languages.end(), language) ==
          ready->languages.end())
    return fail(Outcome::kIncompatible,
                "incompatible MLE worker: it does not run the language '" +
                    std::string(language) + "'",
                error);
  if (m_dead) return died(error);
  m_languages = ready->languages;
  m_minor = std::min(wire::version_minor(version), wire::kVersionMinor);
  m_ready = true;
  return Outcome::kOk;
}

// ---------------------------------------------------------------- requests

std::string Worker::check_result(const Request &q, uint32_t status,
                                 const ResultMsg &r) const {
  const auto s = static_cast<Status>(status);
  const bool ok = s == Status::kOk;
  const bool call = q.op == Op::kCall;
  if ((q.op == Op::kDrop || q.op == Op::kReset) && !ok &&
      s != Status::kInternal)
    return "a status that is not allowed for the request";
  if (q.op == Op::kOptions && !ok && s != Status::kInternal &&
      s != Status::kGuestError)
    return "a status that is not allowed for the request";
  if (ok == r.error.has_value()) return "RESULT status and error disagree";
  if (s == Status::kInterrupted && !interrupted())
    return "INTERRUPTED without an interrupt";
  const bool expect_return = call && ok && q.routine->has_return;
  if (r.ret.has_value() != expect_return)
    return r.ret ? "unexpected return value" : "missing return value";
  if (call && ok) {
    // One entry for every OUT and INOUT parameter, ascending.
    static const std::vector<uint32_t> kNone;
    const auto &outs = q.routine->outs ? *q.routine->outs : kNone;
    if (!std::equal(r.out_params.begin(), r.out_params.end(), outs.begin(),
                    outs.end(),
                    [](const auto &p, uint32_t index) {
                      return p.first == index;
                    }))
      return "bad OUT parameters";
  } else if (!r.out_params.empty()) {
    return "unexpected OUT parameters";
  }
  if (q.op != Op::kPrepare && !call &&
      (!r.console_out.empty() || !r.console_err.empty() || r.console_cleared))
    return "console output where none is allowed";
  if (!(q.op == Op::kPrepare && q.check && ok) && !r.findings.empty())
    return "findings where none are allowed";
  if (r.error) {
    const std::string &language = r.error->language;
    const bool own = language.empty() ||
                     std::find(m_languages.begin(), m_languages.end(),
                               language) != m_languages.end();
    bool legal = own;
    if (s == Status::kGuestError) {
      if (call || q.op == Op::kPrepare)
        legal = own || language == wire::kLanguageType;
      if (q.op == Op::kOptions)
        legal = language == wire::kLanguageTimeZone ||
                language == wire::kLanguageOption;
    }
    if (!legal) return "an Error.language that is not allowed";
  }
  return {};
}

Outcome Worker::request(const Request &q, CallHooks &hooks, ResultMsg *result,
                        std::string *error) {
  result->clear();
  forget_record();
  if (!m_usable) return died(error);
  auto refuse = [&](std::string why) {
    *error = std::move(why);
    return Outcome::kRefused;
  };
  if (!m_ready) return refuse("the MLE worker is not ready");
  const bool top = m_depth == 0;
  const bool interruptible = is_interruptible(q.op);
  if (!top) {
    if (q.op != Op::kPrepare && q.op != Op::kCall && q.op != Op::kDrop)
      return refuse(std::string(op_name(q.op)) +
                    " is a top-level request only");
    // A nested request is sent while the worker waits for an SQL_RESULT,
    // and at no other time (§8.1).
    if (m_sql_hooks != m_depth)
      return refuse("a nested request is not possible here");
    if (m_depth > wire::kMaxDepth)
      return refuse("too many nested routine calls (the limit is " +
                    std::to_string(wire::kMaxDepth + 1) + ")");
  }
  if (interruptible && !m_options_in_force)
    return refuse("the MLE worker has no options in force");
  if (q.body.size() >
      (interruptible ? wire::kMaxHostMessage : wire::kMaxSmallMessage))
    return refuse("the request is larger than MLE allows");

  if (top) {
    // Ids whose nested DROP or PREPARE was not sent are dropped first.
    if (!m_unsent_drops.empty() && q.op != Op::kReset) {
      const std::vector<uint64_t> ids(m_unsent_drops.begin(),
                                      m_unsent_drops.end());
      m_unsent_drops.clear();
      for (const uint64_t id : ids)
        if (const Outcome o = drop(id, error); o != Outcome::kOk)
          return m_usable ? o : died(error);
    }
    // A session that is already killed sends nothing (§11.1).
    if (interruptible && hooks.killed()) {
      *error = "interrupted";
      return Outcome::kInterrupted;
    }
    m_kill_hooks = interruptible ? &hooks : nullptr;
    m_noticed = false;
    m_clock_running = false;
    m_clock_used = 0;
    // The held errors of the last request are forgotten by its call_seq.
    m_error_ids = 0;
    m_retire_after_top = false;
  } else {
    // From the moment of notice no nested request is sent (§11.1 step 2).
    std::string why;
    if (tick(nullptr, false, 0, &why) != Wait::kMessage)
      return lost(Wait::kKilled, q.op, why, error);
    if (m_noticed) {
      clock_stop(now_ns());
      if (q.op != Op::kCall) {
        // The id counts as not prepared; a top-level DROP follows later.
        m_routines.erase(q.routine_id);
        m_unsent_drops.insert(q.routine_id);
      }
      *error = "interrupted";
      return Outcome::kInterrupted;
    }
  }

  // The depth, the running request and the clock, restored on every return.
  // Waiting for a nested RESULT counts on the grace clock; the rest of
  // CallHooks::sql does not.
  struct Frame {
    Worker *w;
    bool top;
    Frame(Worker *worker, bool is_top) : w(worker), top(is_top) {
      ++w->m_depth;
      if (!top) w->clock_start(w->m_noticed ? now_ns() : 0);
    }
    ~Frame() {
      --w->m_depth;
      if (top) {
        // A request that ended without its RESULT is over as well.
        if ((w->m_interrupt.load(std::memory_order_relaxed) & kRunning) != 0)
          w->end_running();
        w->m_trace = nullptr;
        w->m_kill_hooks = nullptr;
      } else {
        w->clock_stop(w->m_noticed ? now_ns() : 0);
      }
    }
  } frame(this, top);

  SqlTrace trace;
  if (top && q.op == Op::kCall && sql_trace_enabled()) m_trace = &trace;
  const int64_t trace_start = m_trace == &trace ? now_ns() : 0;
  const int64_t deadline =
      interruptible
          ? 0
          : now_ns() + int64_t{m_options.control_timeout_ms} * 1000000;
  std::string why;
  Wait w = send(q.op, q.body, &hooks, top, interruptible, deadline, &why);
  if (w != Wait::kMessage) return lost(w, q.op, why, error);

  Expect expect;
  expect.ops = bit(Op::kResult);
  if (q.op == Op::kPrepare || q.op == Op::kCall)
    expect.ops |= bit(Op::kSql) | bit(Op::kLibrary);
  expect.depth = m_depth - 1;
  expect.call_seq = m_seq;
  expect.request = q.op;
  while (true) {
    Message m;
    Reservation reservation(&hooks, &m.reserved);
    const int64_t t_wait = m_trace == &trace ? now_ns() : 0;
    w = receive(&m, &hooks, expect, deadline, &why, true);
    if (m_trace == &trace) trace.worker += now_ns() - t_wait;
    if (w != Wait::kMessage) return lost(w, q.op, why, error);

    if (m.h.op == static_cast<uint32_t>(Op::kSql)) {
      w = serve_sql(q, m, hooks, deadline, &why);
    } else if (m.h.op == static_cast<uint32_t>(Op::kLibrary)) {
      w = serve_library(m, hooks, deadline, &why);
    } else {
      // The request is complete: no interrupt is recorded for it any more.
      if (top && interruptible) end_running();
      // TAINTED (§5.1): the worker asks to be retired when the top-level
      // request is over; this request keeps its outcome.
      if ((m.h.flags & wire::kTainted) != 0 && !m_retire_after_top) {
        m_retire_after_top = true;
        m_retire_why =
            "the MLE worker was retired: it reported that it must not be "
            "used again";
      }
      // From the region, each byte once; nothing was posted since.
      if (!decode(m.data, result, &why))
        return lost(Wait::kProtocol, q.op, "bad RESULT: " + why, error);
      if (std::string bad = check_result(q, m.h.status, *result); !bad.empty())
        return lost(Wait::kProtocol, q.op, bad, error);
      auto outcome = static_cast<Outcome>(m.h.status);
      if (outcome == Outcome::kOk && q.op == Op::kCall &&
          !((!result->ret || is_finite(*result->ret)) &&
            std::all_of(result->out_params.begin(), result->out_params.end(),
                        [](const auto &p) { return is_finite(p.second); }))) {
        // The host is the checker of floats: a value that is not finite
        // fails the request as a conversion error, and the worker lives on
        // (§7.2, §9.4).
        result->ret.reset();
        result->out_params.clear();
        result->error = Error{wire::kLanguageType,
                              "the routine returned a number that is not "
                              "finite (NaN or an infinity)",
                              {},
                              0,
                              "",
                              0};
        outcome = Outcome::kGuestError;
      }
      if (result->error) *error = result->error->message;
      if (m_trace == &trace) trace.print(now_ns() - trace_start);
      // Retirement (§11.5); the request keeps its outcome.
      if (m_dead) {
        died(nullptr);
      } else if (outcome == Outcome::kInternal && !interruptible) {
        m_retire_after_top = true;
        m_retire_why = std::string("the MLE worker was retired: it answered "
                                   "INTERNAL to ") +
                       op_name(q.op);
      }
      if (top && m_usable && m_retire_after_top)
        fail(Outcome::kDied, m_retire_why, nullptr);
      return outcome;
    }
    // A nested request that failed has ended the worker already.
    if (!m_usable) return died(error);
    if (w != Wait::kMessage) return lost(w, q.op, why, error);
  }
}

Worker::Wait Worker::serve_sql(const Request &q, Message &m, CallHooks &hooks,
                               int64_t deadline, std::string *error) {
  using wire::SqlAction;
  // This depth's message, reused; what a large one held is not kept.
  SqlMsg &sql = m_sql_msgs[std::min<size_t>(m_depth, m_sql_msgs.size() - 1)];
  struct Release {
    SqlMsg &sql;
    ~Release() {
      sql.params.clear();
      if (sql.statement.capacity() > (1U << 20)) std::string().swap(sql.statement);
      if (sql.params.capacity() > 4096) std::vector<Value>().swap(sql.params);
    }
  } release{sql};
  sql.action = m.sql_action;
  sql.stmt_id = m.sql_stmt_id;
  // The head was read by receive(); a discarded message has nothing more
  // (§5.3).
  if (m.discarded) {
    sql.statement.clear();
    sql.params.clear();
    sql.flags = 0;
    sql.max_rows = 0;
    sql.row_width = 0;
  } else if (std::string why;
             !decode_sql_rest(m.data.substr(wire::kSqlHeadSize), &sql, &why)) {
    *error = "bad SQL: " + why;
    return Wait::kProtocol;
  }
  const bool close = sql.action == SqlAction::kClose;
  const bool many = sql.action == SqlAction::kExecuteMany;
  SqlResultMsg r;
  // A result's rows may be written straight into the arena: the request
  // was copied out, so it may be overwritten.
  DirectReply direct;
  direct.data = reinterpret_cast<char *>(m_process->arena());
  direct.capacity = m_process->arena_size();
  direct.posts = &m_posts;

  // The id the reply has if it is an ERROR (§10.4): taken now, so that the
  // hook knows it, while the nested requests the hook runs take their own.
  // An id that no ERROR uses is skipped.
  const uint64_t error_id = ++m_error_ids;
  // Rows of an EXECUTE_MANY executed before an ERROR the host makes.
  uint32_t executed = 0;
  // The action was given to the hook.
  bool ran = false;
  auto refuse = [&](SqlRefusal why) {
    r = SqlResultMsg();
    direct.length = 0;
    r.kind = SqlResultKind::kError;
    r.failed_row = many ? executed : wire::kNoRow;
    hooks.sql_refused(why, ran, sql.action, sql.stmt_id, error_id, &r);
    r.kind = SqlResultKind::kError;
  };

  // The refusals that come before a statement runs (§10.1): after an
  // interrupt and while serving PREPARE all SQL, else what cannot be run.
  std::optional<SqlRefusal> refusal;
  if (m_noticed)
    refusal = SqlRefusal::kInterrupted;
  else if (q.op == Op::kPrepare)
    refusal = SqlRefusal::kPreparing;
  else if (m.discarded)
    refusal = SqlRefusal::kRequestTooLarge;
  else if (!all_finite(sql.params))
    refusal = SqlRefusal::kNonFiniteParameter;

  int64_t t = m_trace != nullptr ? now_ns() : 0;
  if (refusal) {
    refuse(*refusal);
  } else {
    // The statement's time is the server's, not the worker's.
    clock_stop(m_noticed ? now_ns() : 0);
    ++m_sql_hooks;
    hooks.sql_into(sql, &r, &direct, error_id);  // may run nested requests
    --m_sql_hooks;
    ran = true;
    if (!m_usable) return Wait::kDied;
    clock_start(m_noticed ? now_ns() : 0);
    if (m_trace != nullptr) {
      const int64_t t2 = now_ns();
      m_trace->sql += t2 - t;
      m_trace->exec += int64_t(r.exec_us) * 1000;
      m_trace->requests++;
      if (!r.columns.empty())
        m_trace->rows += r.rows.size() / r.columns.size();
      m_trace->rows += direct.rows;
      t = t2;
    }
    if (const Wait k = tick(nullptr, false, 0, error); k != Wait::kMessage)
      return k;
    // An interrupt noticed meanwhile makes the reply an ERROR, also when
    // the statement succeeded (§11.1 step 2).
    if (m_noticed && !close &&
        (direct.length > 0 || r.kind != SqlResultKind::kError)) {
      if (many && sql.row_width != 0)
        executed = static_cast<uint32_t>(sql.params.size() / sql.row_width);
      refuse(SqlRefusal::kInterrupted);
    }
  }
  if (close) {
    // CLOSE is answered OK in every situation.
    r = SqlResultMsg();
    r.kind = SqlResultKind::kOk;
    direct.length = 0;
  }

  if (direct.length > 0) {
    // Nested requests (which use the arena) only run before the rows are
    // written: during the statement's execution.
    if (direct.posts_at_start != m_posts || direct.length > direct.capacity) {
      fail(Outcome::kInternal,
           "a direct SQL result was overwritten in the mailbox", nullptr);
      return Wait::kDied;
    }
    if (m_trace != nullptr) m_trace->bytes += direct.length;
    m_host_transfer_open = false;
    m_last_host_total = direct.length;
    post(Op::kSqlResult, 0, direct.data, direct.length, direct.length);
    if (m_trace != nullptr) m_trace->send += now_ns() - t;
    return Wait::kMessage;
  }
  // What the worker does not check (§1): an ERROR as §10.4 has it, with
  // its id.
  const auto complete_error = [&] {
    if (r.sql_errno < 1 || r.sql_errno > wire::kMaxSqlErrno) r.sql_errno = 1105;
    if (r.sqlstate.size() != 5 || !is_valid_sqlstate(r.sqlstate))
      r.sqlstate = "HY000";
    repair_utf8(&r.message);
    r.error_id = error_id;
  };
  std::string &reply = buffer(m_reply_bodies);
  if (r.kind == SqlResultKind::kError) complete_error();
  encode(r, &reply);
  if (reply.size() > wire::kMaxHostMessage) {
    // Decided before the first chunk is posted (§5.3).
    if (many && sql.row_width != 0)
      executed = static_cast<uint32_t>(sql.params.size() / sql.row_width);
    refuse(SqlRefusal::kReplyTooLarge);
    complete_error();
    encode(r, &reply);
  }
  if (r.kind == SqlResultKind::kError) hold_error(r);
  if (m_trace != nullptr) {
    const int64_t t2 = now_ns();
    m_trace->encode += t2 - t;
    m_trace->bytes += reply.size();
    t = t2;
  }
  const Wait w =
      send(Op::kSqlResult, reply, &hooks, false, false, deadline, error);
  if (m_trace != nullptr) m_trace->send += now_ns() - t;
  return w;
}

Worker::Wait Worker::serve_library(Message &m, CallHooks &hooks,
                                   int64_t deadline, std::string *error) {
  LibraryMsg library;
  if (std::string why; !decode(m.data, &library, &why)) {
    *error = "bad LIBRARY: " + why;
    return Wait::kProtocol;
  }
  LibraryResultMsg r;
  auto refuse = [&r](const char *why) {
    r = LibraryResultMsg();
    r.message = why;
  };
  if (m_noticed) {
    refuse("the statement was interrupted");
  } else {
    clock_stop(0);
    hooks.library(library, &r);
    if (!m_usable) return Wait::kDied;
    if (const Wait k = tick(nullptr, false, 0, error); k != Wait::kMessage)
      return k;
    if (m_noticed) refuse("the statement was interrupted");
  }
  // What the worker does not check (§1): a body and a language as §9.10
  // has them, a message that is well-formed.
  if (r.status != wire::LibraryStatus::kRefused &&
      (!is_valid_identifier(r.language) || !is_valid_utf8(r.body) ||
       r.version == 0 || !r.message.empty() ||
       (r.status == wire::LibraryStatus::kUnchanged &&
        (library.known_version == 0 || r.version != library.known_version ||
         !r.body.empty()))))
    refuse("the library is not available");
  repair_utf8(&r.message);
  std::string reply;
  encode(r, &reply);
  if (reply.size() > wire::kMaxHostMessage) {
    refuse("the library is larger than MLE allows");
    encode(r, &reply);
  }
  return send(Op::kLibraryResult, reply, &hooks, false, false, deadline, error);
}

Outcome Worker::parse_library(const ParseLibraryMsg &library, CallHooks &hooks,
                              ResultMsg *result, std::string *error) {
  // The host sends no language the worker did not announce (§1 item 4).
  if (m_ready && std::find(m_languages.begin(), m_languages.end(),
                           library.language) == m_languages.end()) {
    forget_record();
    *error = "the MLE worker does not run the language '" + library.language + "'";
    return Outcome::kRefused;
  }
  std::string body;
  encode(library, &body);
  return request(Request{Op::kParseLibrary, body}, hooks, result, error);
}

Outcome Worker::prepare(const PrepareMsg &routine, CallHooks &hooks,
                        ResultMsg *result, std::string *error) {
  if (routine.params.size() > wire::kMaxParams ||
      routine.imports.size() > wire::kMaxImports) {
    forget_record();
    *error = "the routine has more parameters or imports than MLE allows";
    return Outcome::kRefused;
  }
  // The host sends no language the worker did not announce (§1 item 4).
  if (m_ready && std::find(m_languages.begin(), m_languages.end(),
                           routine.language) == m_languages.end()) {
    forget_record();
    *error = "the MLE worker does not run the language '" + routine.language + "'";
    return Outcome::kRefused;
  }
  std::string body;
  encode(routine, &body);
  Request q{Op::kPrepare, body};
  q.routine_id = routine.routine_id;
  q.check = routine.check;
  const Outcome o = request(q, hooks, result, error);
  if (o == Outcome::kOk) {
    // A DROP of the id that still waits to be sent is for what the id was
    // before.
    m_unsent_drops.erase(routine.routine_id);
    RoutineInfo info{routine.params.size(), routine.returns.has_value(), {}};
    std::vector<uint32_t> outs;
    for (uint32_t i = 0; i < routine.params.size(); i++)
      if (routine.params[i].mode != wire::ParamMode::kIn) outs.push_back(i);
    if (!outs.empty())
      info.outs = std::make_shared<const std::vector<uint32_t>>(std::move(outs));
    m_routines[routine.routine_id] = std::move(info);
  } else if (o != Outcome::kRefused) {
    // A PREPARE that was sent and did not answer OK leaves the id not
    // prepared, whatever it was before (§8.3).
    m_routines.erase(routine.routine_id);
  }
  return o;
}

Outcome Worker::call(uint64_t routine_id, uint64_t library_epoch,
                     const std::vector<Value> &args, CallHooks &hooks,
                     ResultMsg *result, std::string *error) {
  const auto it = m_routines.find(routine_id);
  if (it == m_routines.end() || it->second.params != args.size()) {
    forget_record();
    *error = "routine " + std::to_string(routine_id) +
             " is not prepared with " + std::to_string(args.size()) +
             " parameters";
    return Outcome::kRefused;
  }
  // The RESULT is checked against the parameters in force now: a nested
  // PREPARE or DROP may change the entry meanwhile (a copy without
  // allocation: the list of OUT parameters is shared).
  const RoutineInfo info = it->second;
  // One buffer per depth: a nested call is made while the outer one waits.
  std::string &body = buffer(m_call_bodies);
  Writer w(&body);
  w.u64(routine_id);
  w.u64(library_epoch);
  w.u32(static_cast<uint32_t>(args.size()));
  for (const Value &v : args) w.value(v);
  Request q{Op::kCall, body};
  q.routine_id = routine_id;
  q.routine = &info;
  return request(q, hooks, result, error);
}

Outcome Worker::drop(uint64_t routine_id, std::string *error) {
  if (m_usable && m_ready && m_depth != 0 && m_sql_hooks != m_depth) {
    // No request can be sent now: the id is not prepared from here on, and
    // its DROP goes out before the next top-level request.
    m_routines.erase(routine_id);
    m_unsent_drops.insert(routine_id);
    if (error != nullptr) error->clear();
    return Outcome::kOk;
  }
  std::string body;
  encode(DropMsg{routine_id}, &body);
  NoSql hooks;
  ResultMsg result;
  Request q{Op::kDrop, body};
  q.routine_id = routine_id;
  const Outcome o = request(q, hooks, &result, error);
  if (o != Outcome::kRefused) m_routines.erase(routine_id);
  return o;
}

Outcome Worker::send_pending_drops(std::string *error) {
  if (m_unsent_drops.empty()) return Outcome::kOk;
  if (!m_usable) return died(error);
  if (m_depth != 0) {
    *error = "DROP is sent at top level only here";
    return Outcome::kRefused;
  }
  const std::vector<uint64_t> ids(m_unsent_drops.begin(), m_unsent_drops.end());
  m_unsent_drops.clear();
  for (const uint64_t id : ids)
    if (const Outcome o = drop(id, error); o != Outcome::kOk)
      return m_usable ? o : died(error);
  return Outcome::kOk;
}

Outcome Worker::options(const OptionsMsg &options, Error *guest_error,
                        std::string *error) {
  if (std::string why; !is_valid(options, &why)) {
    forget_record();
    *error = "bad options: " + why;
    return Outcome::kRefused;
  }
  // Options never change under prepared routines: OPTIONS goes only to a
  // worker in which no id is prepared (§9.8).
  if (!m_routines.empty()) {
    forget_record();
    *error = "OPTIONS is only for a worker in which nothing is prepared";
    return Outcome::kRefused;
  }
  std::string body;
  encode(options, &body);
  NoSql hooks;
  ResultMsg result;
  const Outcome o = request(Request{Op::kOptions, body}, hooks, &result, error);
  if (o == Outcome::kOk) m_options_in_force = true;
  if (o == Outcome::kGuestError && guest_error != nullptr)
    *guest_error = *result.error;
  return o;
}

Outcome Worker::reset(std::string *error) {
  NoSql hooks;
  ResultMsg result;
  const Outcome o = request(Request{Op::kReset, {}}, hooks, &result, error);
  if (o == Outcome::kOk) {
    m_routines.clear();
    m_unsent_drops.clear();
    m_options_in_force = false;
  }
  return o;
}

void Worker::interrupt() {
  // The record is made before the send and stands if the send fails. It is
  // made only on a word that says "running s": between the post of the
  // first chunk of s and the receipt of the last chunk of its RESULT.
  uint64_t state = m_interrupt.load(std::memory_order_acquire);
  while (true) {
    if ((state & kRunning) == 0) return;
    // Recorded already: the INTERRUPT is sent once more.
    if ((state & kRequested) != 0) break;
    if (m_interrupt.compare_exchange_weak(state, state | kRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
      break;
  }
  const uint64_t seq = state >> 2;
  unsigned char msg[wire::kInterruptSize];
  msg[0] = static_cast<unsigned char>(wire::Control::kInterrupt);
  memcpy(msg + 1, &seq, sizeof seq);
  // Without blocking and without a signal: a worker that does not take its
  // INTERRUPT is ended when the grace period is over.
  [[maybe_unused]] ssize_t n = ::send(m_process->control_fd(), msg, sizeof msg,
                                      MSG_NOSIGNAL | MSG_DONTWAIT);
}

}  // namespace mle
