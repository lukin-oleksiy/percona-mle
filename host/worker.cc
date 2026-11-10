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

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>

namespace mle {

namespace {

constexpr int kTickMs =
    10;  // futex timeout: how often KILL and death are checked
constexpr int kExitReportMs = 1000;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

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
  switch (static_cast<wire::Op>(op)) {
    case wire::Op::kReady:
      return "READY";
    case wire::Op::kPrepare:
      return "PREPARE";
    case wire::Op::kCall:
      return "CALL";
    case wire::Op::kResult:
      return "RESULT";
    case wire::Op::kSql:
      return "SQL";
    case wire::Op::kSqlResult:
      return "SQL_RESULT";
    case wire::Op::kFetch:
      return "FETCH";
    case wire::Op::kDrop:
      return "DROP";
    case wire::Op::kReset:
      return "RESET";
    case wire::Op::kContinue:
      return "CONTINUE";
  }
  return "?";
}

/** Restores the nesting depth and the running call on every return. */
class DepthGuard {
 public:
  DepthGuard(uint32_t *depth, std::atomic<uint64_t> *running)
      : m_depth(depth), m_running(running), m_top(*depth == 0) {
    ++*m_depth;
  }
  ~DepthGuard() {
    --*m_depth;
    if (m_top) m_running->store(0);
  }
  bool top() const { return m_top; }

 private:
  uint32_t *m_depth;
  std::atomic<uint64_t> *m_running;
  bool m_top;
};

}  // namespace

void NoSql::sql(const SqlMsg &, SqlResultMsg *result) {
  result->kind = SqlResultKind::kError;
  result->sql_errno = 0;
  result->sqlstate = "HY000";
  result->message = "SQL is not allowed here";
}

Worker::Worker(std::unique_ptr<WorkerProcess> process,
               const WorkerOptions &options)
    : m_process(std::move(process)), m_options(options) {}

Worker::~Worker() = default;  // the WorkerProcess kills the worker

// ---------------------------------------------------------------- failures

Status Worker::fail(Status status, const std::string &why, std::string *error) {
  m_process->kill();
  m_usable = false;
  m_failure = why;
  if (error != nullptr) *error = why;
  return status;
}

Status Worker::died(std::string *error) {
  if (m_usable) {
    m_usable = false;
    m_failure = "the MLE worker died: " +
                m_process->exit_info(kExitReportMs).describe();
  }
  if (error != nullptr) *error = m_failure;
  return Status::kDied;
}

WorkerStats Worker::stats() const {
  const wire::WorkerStats &s = m_process->header()->stats;
  WorkerStats r;
  r.heap_max = s.heap_max;
  r.heap_used = std::min(s.heap_used, s.heap_max);
  r.state = s.state <= uint32_t(wire::WorkerState::kCompiling) ? s.state : 0;
  r.tainted = s.tainted != 0;
  return r;
}

// ---------------------------------------------------------------- mailbox

void Worker::post(wire::Op op, uint32_t flags, const char *data, size_t len,
                  uint64_t total) {
  wire::RegionHeader *h = m_process->header();
  if (len > 0) memcpy(m_process->arena(), data, len);
  wire::MessageHeader m{};
  m.op = static_cast<uint32_t>(op);
  m.flags = flags;
  m.depth = m_depth == 0 ? 0 : m_depth - 1;
  m.len = static_cast<uint32_t>(len);
  m.total_len = total;
  m.call_seq = m_seq;
  memcpy(&h->msg, &m, sizeof m);
  h->to_w.fetch_add(1, std::memory_order_seq_cst);  // publishes the above
  if (h->w_sleep.load(std::memory_order_seq_cst) != 0) futex_wake(&h->to_w);
}

Worker::Wait Worker::wait_change(CallHooks *hooks, int timeout_ms,
                                 std::string *error) {
  wire::RegionHeader *h = m_process->header();
  const int64_t start = now_ns();
  const int64_t spin_until = start + int64_t{m_options.spin_us} * 1000;
  const int64_t deadline = timeout_ms < 0
                               ? std::numeric_limits<int64_t>::max()
                               : start + int64_t{timeout_ms} * 1000000;
  for (uint32_t i = 0;; i++) {
    const uint32_t v = h->to_h.load(std::memory_order_acquire);
    if (v != m_seen_h) {
      m_seen_h = v;
      return Wait::kMessage;
    }
    if ((i & 63) == 0 && now_ns() >= spin_until) break;
    cpu_relax();
  }
  while (true) {
    h->h_sleep.store(1, std::memory_order_seq_cst);
    if (h->to_h.load(std::memory_order_seq_cst) == m_seen_h)
      futex_wait(&h->to_h, m_seen_h, kTickMs);
    h->h_sleep.store(0, std::memory_order_seq_cst);
    const uint32_t v = h->to_h.load(std::memory_order_acquire);
    if (v != m_seen_h) {
      m_seen_h = v;
      return Wait::kMessage;
    }
    // A tick: is the worker alive, was the session killed, time is up?
    if (!m_process->alive()) return Wait::kDied;
    if (check_kill(hooks) == Wait::kKilled) return Wait::kKilled;
    if (now_ns() > deadline) {
      if (error != nullptr) *error = "timed out waiting for the MLE worker";
      return Wait::kTimeout;
    }
  }
}

Worker::Wait Worker::check_kill(CallHooks *hooks) {
  if (hooks == nullptr || m_running.load() == 0 || !hooks->killed())
    return Wait::kMessage;
  const int64_t now_ms = now_ns() / 1000000;
  if (m_interrupted_at_ms == 0) {
    interrupt();
    m_interrupted_at_ms = now_ms;
  } else if (now_ms - m_interrupted_at_ms > m_options.interrupt_grace_ms) {
    m_process->kill();
    return Wait::kKilled;
  }
  return Wait::kMessage;
}

Worker::Wait Worker::receive(Message *m, CallHooks *hooks, int timeout_ms,
                             std::string *error) {
  Assembler a(m_process->arena_size(), m_options.max_message);
  while (true) {
    const Wait w = wait_change(hooks, timeout_ms, error);
    if (w != Wait::kMessage) return w;
    // Copy out first: the worker may change shared memory at any time.
    wire::MessageHeader h;
    memcpy(&h, &m_process->header()->msg, sizeof h);
    std::string why;
    if (!check_worker_header(h, m_process->arena_size(), &why)) {
      *error = "the MLE worker broke the protocol: " + why;
      return Wait::kProtocol;
    }
    switch (a.add(h, m_process->arena())) {
      case Assembler::Step::kDone:
        m->h = a.header();
        m->body = a.body();
        return Wait::kMessage;
      case Assembler::Step::kError:
        *error = "the MLE worker broke the protocol: " + a.error();
        return Wait::kProtocol;
      case Assembler::Step::kNeedMore:
        post(wire::Op::kContinue, 0, nullptr, 0, 0);
        break;
    }
  }
}

Worker::Wait Worker::send(wire::Op op, const std::string &body,
                          CallHooks *hooks, std::string *error) {
  const uint32_t arena = m_process->arena_size();
  size_t sent = 0;
  while (true) {
    const size_t len = std::min<size_t>(arena, body.size() - sent);
    const bool more = sent + len < body.size();
    post(op, more ? uint32_t{wire::kMore} : 0U, body.data() + sent, len,
         body.size());
    sent += len;
    if (!more) return Wait::kMessage;
    const Wait w = wait_change(hooks, -1, error);
    if (w != Wait::kMessage) return w;
    wire::MessageHeader h;
    memcpy(&h, &m_process->header()->msg, sizeof h);
    if (h.op != uint32_t(wire::Op::kContinue) || h.len != 0 ||
        h.call_seq != m_seq || h.depth != m_depth - 1) {
      *error = "the MLE worker broke the protocol: expected CONTINUE";
      return Wait::kProtocol;
    }
  }
}

// ---------------------------------------------------------------- requests

Status Worker::wait_ready(int timeout_ms, ReadyMsg *ready, std::string *error) {
  if (!m_usable) return died(error);
  Message m;
  std::string why;
  switch (receive(&m, nullptr, timeout_ms, &why)) {
    case Wait::kMessage:
      break;
    case Wait::kDied:
      return died(error);
    case Wait::kProtocol:
      return fail(Status::kProtocolError, why, error);
    default:
      return fail(Status::kDied, "no READY from the MLE worker", error);
  }
  if (m.h.op != uint32_t(wire::Op::kReady) || m.h.depth != 0 ||
      m.h.call_seq != 0 || !decode(m.body, ready, &why) ||
      ready->version != wire::kVersion)
    return fail(Status::kProtocolError,
                "the MLE worker broke the protocol: bad READY " + why, error);
  if (m.h.status != uint32_t(Status::kOk))
    return fail(Status::kInternal,
                "the MLE worker failed to start: " + ready->info, error);
  m_ready = true;
  return Status::kOk;
}

Status Worker::request(wire::Op op, const std::string &body, CallHooks &hooks,
                       const RoutineInfo *routine, ResultMsg *result,
                       std::string *error) {
  if (!m_usable) return died(error);
  if (!m_ready) {
    *error = "the MLE worker is not ready";
    return Status::kInternal;
  }
  if (m_depth >= m_options.max_depth) {
    *error = "too many nested routine calls (the limit is " +
             std::to_string(m_options.max_depth) + ")";
    return Status::kGuestError;
  }
  if (m_depth == 0) {
    if (hooks.killed()) {
      *error = "interrupted";
      return Status::kInterrupted;
    }
    m_seq++;
    m_interrupted_at_ms = 0;
    m_running.store(m_seq);
  }
  DepthGuard guard(&m_depth, &m_running);
  const uint32_t depth = m_depth - 1;
  std::string why;

  auto broke = [&](const std::string &what) {
    return fail(Status::kProtocolError,
                "the MLE worker broke the protocol: " + what, error);
  };
  auto lost = [&](Wait w) {
    // A worker that ends while a KILL is being delivered was ended by the
    // KILL (e.g. GraalPy exits when an interrupt hits a native extension's
    // callback, U-5): report it as such.
    if (w == Wait::kDied && m_interrupted_at_ms != 0) {
      died(nullptr);
      return fail(Status::kKilled,
                  "the MLE worker ended after KILL (" +
                      m_process->exit_info(kExitReportMs).describe() + ")",
                  error);
    }
    if (w == Wait::kDied) return died(error);
    if (w == Wait::kKilled)
      return fail(Status::kKilled,
                  "the MLE worker did not stop within " +
                      std::to_string(m_options.interrupt_grace_ms) +
                      " ms after KILL and was terminated",
                  error);
    return fail(Status::kProtocolError, why, error);
  };

  Wait w = send(op, body, &hooks, &why);
  if (w != Wait::kMessage) return lost(w);
  while (true) {
    Message m;
    w = receive(&m, &hooks, -1, &why);
    if (w != Wait::kMessage) return lost(w);
    if (m.h.depth != depth || m.h.call_seq != m_seq)
      return broke(std::string(op_name(m.h.op)) + " for the wrong call");

    if (m.h.op == uint32_t(wire::Op::kResult)) {
      if (!decode(m.body, result, &why)) return broke("bad RESULT: " + why);
      const bool ok = m.h.status == uint32_t(Status::kOk);
      if (ok == result->error.has_value())
        return broke("RESULT status and error disagree");
      const bool expect_return = op == wire::Op::kCall && ok &&
                                 routine != nullptr && routine->has_return;
      if (result->ret.has_value() != expect_return)
        return broke(result->ret ? "unexpected return value"
                                 : "missing return value");
      std::set<uint32_t> seen;
      for (const auto &[index, value] : result->out_params)
        if (op != wire::Op::kCall || routine == nullptr ||
            index >= routine->params || !seen.insert(index).second)
          return broke("bad OUT parameter");
      // A worker that ran out of heap or stack is retired once the
      // outermost call is over (an outer call may still be using it).
      if (guard.top() && m_process->header()->stats.tainted != 0)
        fail(Status::kDied,
             "the MLE worker was retired after running out of memory or "
             "stack",
             nullptr);
      return static_cast<Status>(m.h.status);
    }

    if (m.h.op == uint32_t(wire::Op::kSql) && op == wire::Op::kCall) {
      SqlMsg sql;
      if (!decode(m.body, &sql, &why)) return broke("bad SQL: " + why);
      SqlResultMsg r;
      hooks.sql(sql, &r);  // may run nested calls on this worker
      if (!m_usable) return died(error);
      // A routine that keeps the worker busy with SQL never lets the wait
      // reach a tick: check for KILL after every statement too.
      if (check_kill(&hooks) == Wait::kKilled) return lost(Wait::kKilled);
      std::string reply;
      encode(r, &reply);
      w = send(wire::Op::kSqlResult, reply, &hooks, &why);
      if (w != Wait::kMessage) return lost(w);
      continue;
    }
    return broke(std::string("unexpected ") + op_name(m.h.op) + " during " +
                 op_name(uint32_t(op)));
  }
}

Status Worker::prepare(const PrepareMsg &routine, CallHooks &hooks,
                       ResultMsg *result, std::string *error) {
  std::string body;
  encode(routine, &body);
  const Status s =
      request(wire::Op::kPrepare, body, hooks, nullptr, result, error);
  if (s == Status::kOk)
    m_routines[routine.routine_id] = {routine.params.size(),
                                      routine.returns.has_value()};
  return s;
}

Status Worker::call(uint64_t routine_id, uint32_t events,
                    const std::vector<Value> &args, CallHooks &hooks,
                    ResultMsg *result, std::string *error) {
  const auto it = m_routines.find(routine_id);
  if (it == m_routines.end() || it->second.params != args.size()) {
    *error = "routine " + std::to_string(routine_id) +
             " is not prepared with " + std::to_string(args.size()) +
             " parameters";
    return Status::kInternal;
  }
  const RoutineInfo info = it->second;  // a nested DROP may erase it
  std::string body;
  Writer w(&body);
  w.u64(routine_id);
  w.u32(events);
  w.u32(static_cast<uint32_t>(args.size()));
  for (const Value &v : args) w.value(v);
  return request(wire::Op::kCall, body, hooks, &info, result, error);
}

Status Worker::drop(uint64_t routine_id, std::string *error) {
  std::string body;
  encode(DropMsg{routine_id}, &body);
  NoSql hooks;
  ResultMsg result;
  const Status s =
      request(wire::Op::kDrop, body, hooks, nullptr, &result, error);
  if (s == Status::kOk) m_routines.erase(routine_id);
  if (s != Status::kOk && result.error) *error = result.error->message;
  return s;
}

Status Worker::reset(std::string *error) {
  NoSql hooks;
  ResultMsg result;
  const Status s =
      request(wire::Op::kReset, "", hooks, nullptr, &result, error);
  if (s == Status::kOk) m_routines.clear();
  if (s != Status::kOk && result.error) *error = result.error->message;
  return s;
}

void Worker::interrupt() {
  const uint64_t seq = m_running.load();
  if (seq == 0) return;
  unsigned char msg[9];
  msg[0] = static_cast<unsigned char>(wire::Control::kInterrupt);
  memcpy(msg + 1, &seq, sizeof seq);
  // Non-blocking: a worker that does not read its control socket must not
  // block the KILL handler; SIGKILL after the grace covers it.
  [[maybe_unused]] ssize_t n = ::send(m_process->control_fd(), msg, sizeof msg,
                                      MSG_NOSIGNAL | MSG_DONTWAIT);
}

}  // namespace mle
