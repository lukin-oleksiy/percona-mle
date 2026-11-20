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

  A native worker's side of the mailbox (docs/protocol.md; include/mle/protocol.h): the start-up
  checks of §2, turns and waiting (§4), chunked messages both ways with the size limits (§5, §12),
  the dialogue's grammar (§8.1), READY (§9.1), WorkerStats (§3.1), the control socket (§6) and the
  interrupt variables of §11.1. Shared by the CPython and V8 workers; what an engine does around a
  sleep and on an interrupt are its hooks.

  Every protocol failure, here and in the workers, ends the process through
  common::protocol_failure (wire.hpp).
*/

#ifndef MLE_WORKER_COMMON_MAILBOX_HPP
#define MLE_WORKER_COMMON_MAILBOX_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "mle/protocol.h"
#include "wire.hpp"

namespace mle::common {

/** What an engine does at the mailbox's edges; every hook may be empty. */
struct EngineHooks {
  /** Around the futex sleep while waiting for the host (CPython: release and take the GIL). */
  void (*before_sleep)() = nullptr;
  void (*after_sleep)() = nullptr;
  /** Delivers an interrupt of the running top-level request seq (§11.1): make its guest code stop
      soon, at every depth. Called with the interrupt lock held, by the control thread or by
      Interrupts::begin (no function of Interrupts may be called from it); at most once per
      request. */
  void (*interrupt)(uint64_t seq) = nullptr;
  /** The request an interrupt was delivered to has ended: cancel the interrupt if the runtime has
      not acted on it. In the serving thread, from Interrupts::end. */
  void (*interrupted_end)() = nullptr;
};

/** A received message. body is the arena (one chunk: valid until the worker's next write to the
    arena) or the buffer the allocator gave (a message in chunks). */
struct Message {
  uint32_t op;
  uint32_t depth;
  uint64_t call_seq;
  std::span<const std::byte> body;
  bool in_arena;
  /** The worker could not hold the message (the allocator gave no buffer): the transfer was
      completed and its chunks discarded (§5.3); body is empty. The caller fails it locally. */
  bool discarded;
  /** Of a discarded message: the first 8 bytes of the body (a PREPARE's routine_id). */
  uint64_t head;
};

class Mailbox {
 public:
  /** Maps the region (fd 3) after the checks of §2 and starts the control thread (fd 4); returns
      the arena's size. Ends the process with status 2 if the region is not one; posts a failed
      READY and ends the process if the region has another major version; ends it with status 1 on
      any other failure. name prefixes messages on stderr ("mle_v8_worker"). */
  static uint32_t open(int spin, const EngineHooks &hooks, const char *name);

  /** Posts READY with status OK (§9.1): this version, no features. worker_class and each language
      are 1 to 32 characters of [a-z0-9_]; at most 16 languages; the texts are well-formed UTF-8. */
  static void ready(std::string_view info, std::string_view worker_class,
                    std::span<const std::string_view> languages, std::string_view runtime);
  /** Posts a failed READY (status INTERNAL, info the reason) and ends the process: the runtime
      cannot serve (§2 step 5). info is cut to what READY may carry; bytes outside ASCII become '?'. */
  [[noreturn]] static void failed_ready(std::string_view info);

  /** Waits for the next message of the host and checks its header on the first chunk: the op the
      dialogue allows here, depth, call_seq, sizes (§5.2, §8.1, §12); anything else is a protocol
      failure. A message in chunks goes into allocate(total_len), CONTINUE answered for each chunk;
      if allocate returns nullptr the chunks are discarded (Message::discarded). The buffer needs
      no alignment unless the caller reads VECTOR elements in place (§7.2). */
  static Message receive(const std::function<std::byte *(size_t)> &allocate);

  /** Posts body as one message, in chunks if it is larger than the arena. The message must be one
      the dialogue allows here and within the limits of §12 (64 MiB; READY, LIBRARY and the RESULT
      of DROP, RESET and OPTIONS one chunk of 64 KiB): the caller turns what guest code makes larger
      into an error first. No guest code may run between composing body and this call. */
  static void send(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, std::span<const std::byte> body);

  /** The arena, for a message of one chunk written in place; post() sends its first len bytes. */
  static std::span<std::byte> arena();
  static void post(uint32_t op, uint32_t status, uint32_t depth, uint64_t seq, uint32_t len);

  /** True if the host has posted a message receive() has not taken. */
  static bool pending();

  /** Writes the statistics (§3.1). tainted must be set before the RESULT of the request (§11.5). */
  static void stats(uint32_t state, bool tainted, uint64_t heap_used = 0, uint64_t heap_max = 0,
                    uint64_t gc_count = 0);
};

/** Interruption of top-level requests: the worker's side of §11.1. */
class Interrupts {
 public:
  /** The worker begins to serve top-level request seq (it has received it completely).
      interruptible: PREPARE, CALL, PARSE_LIBRARY. An INTERRUPT that came first is delivered now. */
  static void begin(uint64_t seq, bool interruptible);
  /** The top-level request ends: the status of its RESULT is fixed from here on. True if an
      interrupt was delivered to it (only then may the status be INTERRUPTED); then
      EngineHooks::interrupted_end is called. In the thread that serves requests. */
  static bool end();
  /** Whether seq is the running interruptible request (for an engine's interrupt helper thread). */
  static bool running(uint64_t seq);
  /** An interrupt was delivered to the running top-level request (end() not called yet): guest
      code of it, at any depth, must not run on. */
  static bool interrupted();
};

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_MAILBOX_HPP
