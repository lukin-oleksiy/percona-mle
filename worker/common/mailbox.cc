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

#include "mailbox.hpp"

#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace mle::common {

namespace w = mle::wire;

namespace {

w::RegionHeader *g_hdr = nullptr;
std::byte *g_arena = nullptr;
uint32_t g_arena_size = 0;
/** The posts made: the value of to_h. Kept here, never read back from the region (§4.1). */
uint32_t g_posted = 0;
/** to_w as last seen. */
uint32_t g_seen = 0;
int g_spin = 2000;
EngineHooks g_hooks;
const char *g_name = "mle worker";

// The dialogue (§8.1): what the host may send next follows from what the worker posted last.
enum class Await {
  /** The worker has the turn. */
  kNothing,
  /** Idle: a top-level request. */
  kRequest,
  /** The reply to an SQL message, or a nested request before it. */
  kSqlResult,
  kLibraryResult,
};
Await g_await = Await::kNothing;
/** call_seq of the top-level request being served, or of the last one. */
uint64_t g_seq = 0;
/** Depth of the innermost request being served; -1 when idle. */
int g_depth = -1;
/** The op of the request being served at each depth. */
std::array<uint32_t, w::kMaxDepth + 1> g_requests{};

// Interrupts (§11.1, the worker's side): the call_seq of the interruptible top-level request being
// served (0: none), the highest call_seq begun, an INTERRUPT that came before its request (0:
// none), and whether an interrupt was delivered to the running request. One lock for the control
// thread's decision and begin()/end(): an INTERRUPT is then either for the running request or
// pending for a later one, never delivered after its request ended.
std::mutex g_interrupt_lock;
uint64_t g_running = 0;
uint64_t g_last = 0;
uint64_t g_pending = 0;
bool g_delivered = false;

[[noreturn]] void die(std::string_view kind, std::string_view what, int status) {
  std::string line = std::string(g_name) + ": fatal: ";
  line += kind;
  line += what;
  line += '\n';
  (void)!write(STDERR_FILENO, line.data(), line.size());
  _exit(status);
}

long futex(std::atomic<uint32_t> *word, int op, uint32_t val) {
  // The words are shared between processes: no FUTEX_PRIVATE_FLAG (§4.3).
  return syscall(SYS_futex, reinterpret_cast<uint32_t *>(word), op, val, nullptr, nullptr, 0);
}

void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

/** Waits for the host's post (§4.3): polls, then sleeps on the futex (EngineHooks around it). */
void wait_host() {
  for (int i = 0; i < g_spin; i++) {
    const uint32_t v = g_hdr->to_w.load(std::memory_order_acquire);
    if (v != g_seen) {
      g_seen = v;
      return;
    }
    cpu_relax();
  }
  if (g_hooks.before_sleep != nullptr) g_hooks.before_sleep();
  while (true) {
    g_hdr->w_sleep.store(1, std::memory_order_seq_cst);
    if (g_hdr->to_w.load(std::memory_order_seq_cst) == g_seen) futex(&g_hdr->to_w, FUTEX_WAIT, g_seen);
    g_hdr->w_sleep.store(0, std::memory_order_release);
    const uint32_t v = g_hdr->to_w.load(std::memory_order_acquire);
    if (v != g_seen) {
      g_seen = v;
      break;
    }
  }
  if (g_hooks.after_sleep != nullptr) g_hooks.after_sleep();
}

/** Posts what is in the arena under header m (§4.1, §4.2): the header, then the word, then the
    wake-up if the host sleeps. */
void post_raw(const w::MessageHeader &m) {
  g_hdr->msg = m;
  g_hdr->to_h.store(++g_posted, std::memory_order_seq_cst);
  if (g_hdr->h_sleep.load(std::memory_order_seq_cst) != 0) futex(&g_hdr->to_h, FUTEX_WAKE, 1);
}

w::MessageHeader header(uint32_t op, uint32_t status, uint32_t flags, uint32_t depth, uint32_t len, uint64_t total,
                        uint64_t seq) {
  w::MessageHeader m{};
  m.op = op;
  m.status = status;
  m.flags = flags;
  m.depth = depth;
  m.len = len;
  m.total_len = total;
  m.call_seq = seq;
  return m;
}

bool is(uint32_t op, w::Op expected) { return op == static_cast<uint32_t>(expected); }

/** The first chunk of a message of the host: is it what the dialogue allows here (§8.1, §8.2)? */
void check_received(const w::MessageHeader &m) {
  const bool nestable = is(m.op, w::Op::kPrepare) || is(m.op, w::Op::kCall) || is(m.op, w::Op::kDrop);
  switch (g_await) {
    case Await::kRequest:
      if (!(nestable || is(m.op, w::Op::kParseLibrary) || is(m.op, w::Op::kReset) || is(m.op, w::Op::kOptions)))
        protocol_failure("a message that is not a request while idle");
      if (m.depth != 0 || m.call_seq != g_seq + 1) protocol_failure("a top-level request with a wrong depth or call_seq");
      g_seq = m.call_seq;
      g_depth = 0;
      g_requests[0] = m.op;
      break;
    case Await::kSqlResult:
      if (m.call_seq != g_seq) protocol_failure("a message with the call_seq of another request");
      if (is(m.op, w::Op::kSqlResult)) {
        if (m.depth != static_cast<uint32_t>(g_depth)) protocol_failure("an SQL_RESULT of another depth");
      } else if (nestable && is(g_requests[g_depth], w::Op::kCall)) {
        if (m.depth != static_cast<uint32_t>(g_depth) + 1 || m.depth > w::kMaxDepth)
          protocol_failure("a nested request with a wrong depth");
        g_depth++;
        g_requests[g_depth] = m.op;
      } else {
        protocol_failure("a message that is neither the SQL_RESULT nor a nested request");
      }
      break;
    case Await::kLibraryResult:
      if (!is(m.op, w::Op::kLibraryResult) || m.depth != static_cast<uint32_t>(g_depth) || m.call_seq != g_seq)
        protocol_failure("a message that is not the LIBRARY_RESULT");
      break;
    case Await::kNothing:
      protocol_failure("a message of the host while the worker has the turn");
  }
  g_await = Await::kNothing;
  const bool small = is(m.op, w::Op::kOptions) || is(m.op, w::Op::kDrop) || is(m.op, w::Op::kReset);
  if (m.total_len > (small ? w::kMaxSmallMessage : w::kMaxHostMessage)) protocol_failure("a message over the size limit");
}

/** A message the worker is about to post: the dialogue's next state, and the limits of §12 (a
    worker that would break them has a defect: better this exit than a violation). */
void check_sent(uint32_t op, uint32_t depth, uint64_t seq, uint64_t total) {
  if (g_await != Await::kNothing) protocol_failure("worker error: a post out of turn");
  uint64_t limit = w::kMaxWorkerMessage;
  if (is(op, w::Op::kReady)) {
    if (g_posted != 0 || depth != 0 || seq != 0) protocol_failure("worker error: a READY out of place");
    limit = w::kMaxSmallMessage;
    g_await = Await::kRequest;
  } else {
    if (g_depth < 0 || depth != static_cast<uint32_t>(g_depth) || seq != g_seq)
      protocol_failure("worker error: a message with a wrong depth or call_seq");
    const uint32_t request = g_requests[g_depth];
    if (is(op, w::Op::kResult)) {
      if (is(request, w::Op::kDrop) || is(request, w::Op::kReset) || is(request, w::Op::kOptions))
        limit = w::kMaxSmallMessage;
      g_depth--;
      g_await = g_depth < 0 ? Await::kRequest : Await::kSqlResult;
    } else if (!is(request, w::Op::kCall) && !is(request, w::Op::kPrepare)) {
      protocol_failure("worker error: SQL or LIBRARY outside CALL and PREPARE");
    } else if (is(op, w::Op::kSql)) {
      g_await = Await::kSqlResult;
    } else if (is(op, w::Op::kLibrary)) {
      limit = w::kMaxSmallMessage;
      g_await = Await::kLibraryResult;
    } else {
      protocol_failure("worker error: a message a worker does not send");
    }
  }
  if (total > limit) protocol_failure("worker error: a message over the size limit");
}

/** With g_interrupt_lock held. */
void deliver() {
  if (g_delivered) return;
  g_delivered = true;
  if (g_hooks.interrupt != nullptr) g_hooks.interrupt(g_running);
}

/** The control socket (§6): INTERRUPT datagrams; end of file ends the process. */
void *control_thread(void *) {
  std::array<unsigned char, 64> buf;  // larger than any datagram, to know its length
  while (true) {
    const ssize_t n = recv(w::kControlFd, buf.data(), buf.size(), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) _exit(0);  // the host closed its end, or is gone
    if (static_cast<size_t>(n) != w::kInterruptSize || buf[0] != static_cast<uint8_t>(w::Control::kInterrupt))
      continue;
    uint64_t seq;
    std::memcpy(&seq, buf.data() + 1, sizeof seq);
    const std::lock_guard lock(g_interrupt_lock);
    if (g_running != 0 && seq == g_running)
      deliver();
    else if (seq > g_last)
      g_pending = seq;
  }
}

}  // namespace

void protocol_failure(std::string_view what) { die("protocol failure: ", what, 1); }

uint32_t Mailbox::open(int spin, const EngineHooks &hooks, const char *name) {
  g_spin = spin;
  g_hooks = hooks;
  g_name = name;
  struct stat st;
  if (fstat(w::kMailboxFd, &st) != 0) die("the mailbox (fd 3): ", std::strerror(errno), 1);
  // A file without room for the header cannot have region_size as its size.
  if (static_cast<uint64_t>(st.st_size) < w::kHeaderSize) die("fd 3 is not an MLE region", "", w::kExitBadRegion);
  void *h = mmap(nullptr, w::kHeaderSize, PROT_READ, MAP_SHARED, w::kMailboxFd, 0);
  if (h == MAP_FAILED) die("mmap of the mailbox: ", std::strerror(errno), 1);
  const auto *head = static_cast<const w::RegionHeader *>(h);
  const uint32_t magic = head->magic, version = head->version, arena_size = head->arena_size;
  const uint64_t size = head->region_size;
  munmap(h, w::kHeaderSize);
  if (magic != w::kMagic || size != w::kHeaderSize + uint64_t{arena_size} || size != static_cast<uint64_t>(st.st_size))
    die("fd 3 is not an MLE region", "", w::kExitBadRegion);
  if (!w::is_valid_arena_size(arena_size)) die("the region's arena has an illegal size", "", 1);
  void *r = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, w::kMailboxFd, 0);
  if (r == MAP_FAILED) die("mmap of the mailbox: ", std::strerror(errno), 1);
  g_hdr = static_cast<w::RegionHeader *>(r);
  g_arena = static_cast<std::byte *>(r) + w::kHeaderSize;
  g_arena_size = arena_size;
  if (w::version_major(version) != w::kVersionMajor)
    failed_ready("worker speaks " + std::to_string(w::kVersionMajor) + "." + std::to_string(w::kVersionMinor) +
                 ", host " + std::to_string(w::version_major(version)) + "." +
                 std::to_string(w::version_minor(version)));

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 << 10);
  pthread_t t;
  const int err = pthread_create(&t, &attr, control_thread, nullptr);
  pthread_attr_destroy(&attr);
  if (err != 0) die("the control thread: ", std::strerror(err), 1);
  return g_arena_size;
}

void Mailbox::ready(std::string_view info, std::string_view worker_class,
                    std::span<const std::string_view> languages, std::string_view runtime) {
  Writer wr(arena().first(w::kMaxSmallMessage));
  bool ok = wr.put<uint32_t>(w::kVersion) && wr.str(info) && wr.put<uint64_t>(0) && wr.str(worker_class) &&
            wr.put(static_cast<uint32_t>(languages.size()));
  for (const std::string_view language : languages) ok = ok && wr.str(language);
  if (!(ok && wr.str(runtime))) protocol_failure("worker error: a READY over the size limit");
  post(static_cast<uint32_t>(w::Op::kReady), static_cast<uint32_t>(w::Status::kOk), 0, 0,
       static_cast<uint32_t>(wr.size()));
}

void Mailbox::failed_ready(std::string_view info) {
  std::string text(info.substr(0, 4096));
  for (char &c : text)
    if (static_cast<unsigned char>(c) >= 0x80) c = '?';
  Writer wr(arena());
  (void)(wr.put<uint32_t>(w::kVersion) && wr.str(text));
  post(static_cast<uint32_t>(w::Op::kReady), static_cast<uint32_t>(w::Status::kInternal), 0, 0,
       static_cast<uint32_t>(wr.size()));
  die("cannot serve: ", text, 1);
}

Message Mailbox::receive(const std::function<std::byte *(size_t)> &allocate) {
  wait_host();
  const w::MessageHeader first = g_hdr->msg;  // copied once; the checks are made on the copy
  uint64_t len = first.len;
  bool more = (first.flags & w::kMore) != 0;
  const uint64_t total = first.total_len;
  if (!w::is_assigned_op(first.op) || first.status != 0 || (first.flags & ~w::kKnownMessageFlags) != 0 ||
      first.reserved != 0)
    protocol_failure("a malformed message header");
  // §5.2: one chunk exactly when the body fits the arena; every chunk but the last is a whole arena.
  if (more ? len != g_arena_size || total <= g_arena_size : len != total || total > g_arena_size)
    protocol_failure("a message header that breaks the chunk rules");
  check_received(first);
  if (!more) return {first.op, first.depth, first.call_seq, {g_arena, static_cast<size_t>(len)}, true, false, 0};
  uint64_t head = 0;
  std::memcpy(&head, g_arena, sizeof head);  // len is a whole arena here
  std::byte *dst = allocate(static_cast<size_t>(total));
  uint64_t have = 0;
  while (true) {
    if (dst != nullptr) std::memcpy(dst + have, g_arena, len);
    have += len;
    if (!more) break;
    post_raw(header(static_cast<uint32_t>(w::Op::kContinue), 0, 0, first.depth, 0, 0, first.call_seq));
    wait_host();
    const w::MessageHeader m = g_hdr->msg;
    more = (m.flags & w::kMore) != 0;
    len = m.len;
    if (m.op != first.op || m.status != 0 || m.depth != first.depth || m.call_seq != first.call_seq ||
        m.total_len != total || m.reserved != 0 || (m.flags & ~w::kKnownMessageFlags) != 0 ||
        (more ? len != g_arena_size || have + len >= total : len == 0 || len > g_arena_size || have + len != total))
      protocol_failure("a chunk that breaks the chunk rules");
  }
  if (dst == nullptr) return {first.op, first.depth, first.call_seq, {}, false, true, head};
  return {first.op, first.depth, first.call_seq, {dst, static_cast<size_t>(total)}, false, false, 0};
}

void Mailbox::send(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, std::span<const std::byte> body) {
  const uint64_t total = body.size();
  if (total <= g_arena_size) {
    if (total != 0) std::memcpy(g_arena, body.data(), total);
    post(op, status, depth, seq, static_cast<uint32_t>(total));
    return;
  }
  check_sent(op, depth, seq, total);
  for (uint64_t sent = 0;;) {
    const uint64_t len = std::min<uint64_t>(g_arena_size, total - sent);
    std::memcpy(g_arena, body.data() + sent, len);
    const bool more = sent + len < total;
    post_raw(header(op, status, more ? uint32_t{w::kMore} : 0U, depth, static_cast<uint32_t>(len), total, seq));
    sent += len;
    if (!more) break;
    wait_host();
    const w::MessageHeader go = g_hdr->msg;
    if (!is(go.op, w::Op::kContinue) || go.status != 0 || go.flags != 0 || go.len != 0 || go.reserved != 0 ||
        go.total_len != 0 || go.depth != depth || go.call_seq != seq)
      protocol_failure("a message that is not the CONTINUE of a transfer");
  }
}

std::span<std::byte> Mailbox::arena() { return {g_arena, g_arena_size}; }

void Mailbox::post(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, uint32_t len) {
  check_sent(op, depth, seq, len);
  post_raw(header(op, status, 0, depth, len, len, seq));
}

bool Mailbox::pending() { return g_hdr->to_w.load(std::memory_order_acquire) != g_seen; }

void Mailbox::stats(uint32_t state, bool tainted, uint64_t heap_used, uint64_t heap_max, uint64_t gc_count) {
  w::WorkerStats &s = g_hdr->stats;
  s.heap_used = heap_used;
  s.heap_max = heap_max;
  s.gc_count = gc_count;
  s.state = state;
  s.tainted = tainted ? 1 : 0;
}

void Interrupts::begin(uint64_t seq, bool interruptible) {
  const std::lock_guard lock(g_interrupt_lock);
  g_last = seq;
  g_delivered = false;
  if (interruptible) {
    g_running = seq;
    if (g_pending == seq) deliver();
  }
  g_pending = 0;
}

bool Interrupts::end() {
  bool delivered;
  {
    const std::lock_guard lock(g_interrupt_lock);
    g_running = 0;
    delivered = std::exchange(g_delivered, false);
  }
  if (delivered && g_hooks.interrupted_end != nullptr) g_hooks.interrupted_end();
  return delivered;
}

bool Interrupts::interrupted() {
  const std::lock_guard lock(g_interrupt_lock);
  return g_delivered && g_running != 0;
}

bool Interrupts::running(uint64_t seq) {
  const std::lock_guard lock(g_interrupt_lock);
  return g_running != 0 && g_running == seq;
}

}  // namespace mle::common
