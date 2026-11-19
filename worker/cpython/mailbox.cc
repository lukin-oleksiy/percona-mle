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

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <mutex>
#include <system_error>
#include <utility>

namespace mle::cpython {

namespace w = mle::wire;

namespace {

w::RegionHeader *g_hdr = nullptr;
std::byte *g_arena = nullptr;
uint32_t g_arena_size = 0;
uint32_t g_seen_w = 0;
int g_spin = 2000;

// Interrupts: the running top-level request's call_seq (0: none), the last one begun, an INTERRUPT
// that came before its request began, and whether the running one was interrupted. One lock for the
// control thread's decision and begin()/end(): an INTERRUPT is then either for the running request
// (and end() reports it) or pending for a later one, never lost between two stores and never
// tripped after its request ended.
std::mutex g_interrupt_lock;
uint64_t g_running = 0;
uint64_t g_last = 0;
uint64_t g_pending = 0;
bool g_interrupted = false;

[[noreturn]] void fatal(const char *why) {
  fprintf(stderr, "mle_cpython_worker: fatal: %s\n", why);
  _exit(1);
}

long futex(std::atomic<uint32_t> *word, int op, uint32_t val) {
  return syscall(SYS_futex, reinterpret_cast<uint32_t *>(word), op, val, nullptr, nullptr, 0);
}

void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

/** Waits until the host bumps to_w: spin, then sleep on the futex without the GIL. */
void wait_host() {
  for (int i = 0; i < g_spin; i++) {
    const uint32_t v = g_hdr->to_w.load(std::memory_order_acquire);
    if (v != g_seen_w) {
      g_seen_w = v;
      return;
    }
    cpu_relax();
  }
  Py_BEGIN_ALLOW_THREADS;
  while (true) {
    g_hdr->w_sleep.store(1);
    uint32_t v = g_hdr->to_w.load();
    if (v == g_seen_w) futex(&g_hdr->to_w, FUTEX_WAIT, g_seen_w);
    g_hdr->w_sleep.store(0);
    v = g_hdr->to_w.load(std::memory_order_acquire);
    if (v != g_seen_w) {
      g_seen_w = v;
      break;
    }
  }
  Py_END_ALLOW_THREADS;
}

void post_raw() {
  g_hdr->to_h.fetch_add(1);
  if (g_hdr->h_sleep.load() != 0) futex(&g_hdr->to_h, FUTEX_WAKE, 1);
}

void header(uint32_t op, uint32_t status, uint32_t flags, uint32_t depth, uint32_t len, uint64_t total,
            uint64_t seq) {
  w::MessageHeader &m = g_hdr->msg;
  m.op = op;
  m.status = status;
  m.flags = flags;
  m.depth = depth;
  m.len = len;
  m.reserved = 0;
  m.total_len = total;
  m.call_seq = seq;
}

/** With g_interrupt_lock held. */
void interrupt_main() {
  if (g_interrupted) return;
  g_interrupted = true;
  PyErr_SetInterruptEx(SIGINT);  // async-signal-safe; no thread state needed
}

void *control_thread(void *) {
  std::array<unsigned char, 64> buf;
  while (true) {
    const ssize_t n = recv(w::kControlFd, buf.data(), buf.size(), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || buf[0] == uint8_t(w::Control::kShutdown)) _exit(0);  // the host is gone, or ends us
    if (buf[0] != uint8_t(w::Control::kInterrupt) || n < 9) continue;
    uint64_t seq;
    std::memcpy(&seq, buf.data() + 1, sizeof seq);
    const std::lock_guard lock(g_interrupt_lock);
    if (seq == g_running)
      interrupt_main();
    else if (seq > g_last)
      g_pending = seq;
  }
}

}  // namespace

uint32_t Mailbox::open(int spin) {
  g_spin = spin;
  void *h = mmap(nullptr, w::kHeaderSize, PROT_READ, MAP_SHARED, w::kMailboxFd, 0);
  if (h == MAP_FAILED) throw std::system_error(errno, std::system_category(), "mmap of the mailbox");
  const auto *head = static_cast<const w::RegionHeader *>(h);
  const uint32_t magic = head->magic, version = head->version;
  const uint64_t size = head->region_size;
  munmap(h, w::kHeaderSize);
  if (magic != w::kMagic || version != w::kVersion)
    throw std::runtime_error("not an MLE mailbox of protocol version " + std::to_string(w::kVersion));
  void *r = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, w::kMailboxFd, 0);
  if (r == MAP_FAILED) throw std::system_error(errno, std::system_category(), "mmap of the mailbox");
  g_hdr = static_cast<w::RegionHeader *>(r);
  g_arena = static_cast<std::byte *>(r) + w::kHeaderSize;
  g_arena_size = g_hdr->arena_size;
  if (w::region_size(g_arena_size) > size) throw std::runtime_error("the mailbox's arena is larger than its region");

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 << 10);
  pthread_t t;
  const int err = pthread_create(&t, &attr, control_thread, nullptr);
  pthread_attr_destroy(&attr);
  if (err != 0) throw std::system_error(err, std::system_category(), "control thread");
  return g_arena_size;
}

Message Mailbox::receive(const std::function<std::byte *(size_t)> &allocate) {
  wait_host();
  const w::MessageHeader &m = g_hdr->msg;
  const uint32_t op = m.op, status = m.status, depth = m.depth;
  uint64_t len = m.len;
  const uint64_t total = m.total_len, seq = m.call_seq;
  bool more = (m.flags & w::kMore) != 0;
  // The host assembler's checks (codec.cc): one chunk is the whole message, a chunk with more to
  // follow leaves bytes to follow, and the chunks agree on op, depth, sequence and total.
  if (len > g_arena_size || len > total || (more ? len == total : len != total)) fatal("bad message header");
  if (!more) return {op, status, depth, seq, {g_arena, size_t(len)}, true};
  if (total > w::kMaxMessageSize) fatal("message too large");
  std::byte *dst = allocate(size_t(total));
  uint64_t have = 0;
  while (true) {
    std::memcpy(dst + have, g_arena, len);
    have += len;
    if (!more) break;
    header(uint32_t(w::Op::kContinue), 0, 0, depth, 0, 0, seq);
    post_raw();
    wait_host();
    more = (m.flags & w::kMore) != 0;
    len = m.len;
    if (m.op != op || m.call_seq != seq || m.depth != depth || m.total_len != total || len > g_arena_size ||
        have + len > total || (more && have + len == total))
      fatal("bad message chunk");
  }
  if (have != total) fatal("truncated message");
  return {op, status, depth, seq, {dst, size_t(total)}, false};
}

void Mailbox::send(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, std::span<const std::byte> body) {
  const uint64_t total = body.size();
  if (total <= g_arena_size) {
    std::memcpy(g_arena, body.data(), total);
    header(op, status, 0, depth, uint32_t(total), total, seq);
    post_raw();
    return;
  }
  for (uint64_t sent = 0;;) {
    const uint64_t len = std::min<uint64_t>(g_arena_size, total - sent);
    std::memcpy(g_arena, body.data() + sent, len);
    const bool more = sent + len < total;
    header(op, status, more ? uint32_t{w::kMore} : 0U, depth, uint32_t(len), total, seq);
    post_raw();
    sent += len;
    if (!more) return;
    wait_host();
    if (g_hdr->msg.op != uint32_t(w::Op::kContinue)) fatal("expected CONTINUE");
  }
}

std::span<std::byte> Mailbox::arena() { return {g_arena, g_arena_size}; }

void Mailbox::post(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, uint32_t len) {
  header(op, status, 0, depth, len, len, seq);
  post_raw();
}

bool Mailbox::pending() { return g_hdr->to_w.load(std::memory_order_acquire) != g_seen_w; }

void Mailbox::stats(uint32_t state, bool tainted, uint64_t heap_used) {
  w::WorkerStats &s = g_hdr->stats;
  s.heap_used = heap_used;
  s.heap_max = 0;
  s.gc_count = 0;
  s.state = state;
  s.tainted = tainted ? 1 : 0;
}

void Interrupts::begin(uint64_t seq) {
  const std::lock_guard lock(g_interrupt_lock);
  g_interrupted = false;
  g_last = seq;
  g_running = seq;
  if (g_pending == seq) {
    g_pending = 0;
    interrupt_main();
  }
}

bool Interrupts::end() {
  bool interrupted;
  {
    const std::lock_guard lock(g_interrupt_lock);
    g_running = 0;
    interrupted = std::exchange(g_interrupted, false);
  }
  // A KILL tripped in the request's last moments is still pending in the interpreter: taken now,
  // so that it does not raise in the code that sends the RESULT (or in the next request).
  if (interrupted && PyErr_CheckSignals() != 0) PyErr_Clear();
  return interrupted;
}

}  // namespace mle::cpython
