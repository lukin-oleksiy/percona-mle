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

/**
  @file worker/common/mailbox.hpp

  A native worker's side of the shared memory mailbox (include/mle/protocol.h): spin, then sleep on
  the futex; chunked messages both ways; WorkerStats. And the control socket: INTERRUPT of the
  running top-level request, SHUTDOWN or EOF ends the process. Shared by the CPython and V8 workers
  (exp design notes 62, 63); what an engine does around a sleep and on an interrupt are its hooks.
*/

#ifndef MLE_WORKER_COMMON_MAILBOX_HPP
#define MLE_WORKER_COMMON_MAILBOX_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "mle/protocol.h"

namespace mle::common {

/** What an engine does at the mailbox's edges; every hook may be empty. */
struct EngineHooks {
  /** Around the futex sleep while waiting for the host (CPython: release and take the GIL). */
  void (*before_sleep)() = nullptr;
  void (*after_sleep)() = nullptr;
  /** An INTERRUPT for the running top-level request seq: stop the guest code soon. Called by the
      control thread with the interrupt lock held (Interrupts::running(seq) must not be called from
      it); at most once per request. */
  void (*interrupt)(uint64_t seq) = nullptr;
  /** The interrupted request ended: drop an interrupt not delivered yet. In the serving thread. */
  void (*interrupted_end)() = nullptr;
};

/** A received message. body is the arena (one chunk: valid until the worker posts) or the
    buffer the allocator gave (a reassembled message). */
struct Message {
  uint32_t op;
  uint32_t status;
  uint32_t depth;
  uint64_t call_seq;
  std::span<const std::byte> body;
  bool in_arena;
};

class Mailbox {
 public:
  /** Maps the region (fd 3) and starts the control thread (fd 4); the arena's size. name prefixes
      fatal messages ("mle_v8_worker"). */
  static uint32_t open(int spin, const EngineHooks &hooks, const char *name);

  /** Waits for the next message; a message in chunks goes into allocate(total) (answering
      CONTINUE for each chunk). Ends the process on a malformed header (the host is gone or broken). */
  static Message receive(const std::function<std::byte *(size_t)> &allocate);

  /** Posts body as one message, in chunks if it is larger than the arena. */
  static void send(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, std::span<const std::byte> body);

  /** The arena, for messages written in place; post() sends the first len bytes. */
  static std::span<std::byte> arena();
  static void post(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, uint32_t len);

  /** True if the host has posted a message receive() has not taken. */
  static bool pending();

  static void stats(uint32_t state, bool tainted, uint64_t heap_used = 0, uint64_t heap_max = 0,
                    uint64_t gc_count = 0);
};

/** KILL of top-level requests (Control.java's role). */
class Interrupts {
 public:
  /** A top-level request starts; an INTERRUPT that came first applies now. */
  static void begin(uint64_t seq);
  /** The request ended; whether it was interrupted (calls EngineHooks::interrupted_end then). In
      the thread that serves requests. */
  static bool end();
  /** Whether seq is the running request (for an engine's interrupt helper thread). */
  static bool running(uint64_t seq);
  /** The top-level request was interrupted (an INTERRUPT delivered, end() not called yet): a nested
      request that the interrupt stopped may not leave the outer code running. */
  static bool interrupted();
};

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_MAILBOX_HPP
