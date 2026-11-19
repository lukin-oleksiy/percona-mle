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
  @file worker/cpython/mailbox.hpp

  The CPython worker's side of the shared memory mailbox (include/mle/protocol.h): spin, then
  sleep on the futex without the GIL; chunked messages both ways; WorkerStats. And the control
  socket: INTERRUPT of the running top-level request raises KeyboardInterrupt in the main thread
  (PyErr_SetInterruptEx), SHUTDOWN or EOF ends the process.
*/

#ifndef MLE_CPYTHON_MAILBOX_HPP
#define MLE_CPYTHON_MAILBOX_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "mle/protocol.h"

namespace mle::cpython {

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
  /** Maps the region (fd 3) and starts the control thread (fd 4); the arena's size. */
  static uint32_t open(int spin);

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

  static void stats(uint32_t state, bool tainted, uint64_t heap_used = 0);
};

/** KILL of top-level requests (Control.java's role). */
class Interrupts {
 public:
  /** A top-level request starts; an INTERRUPT that came first applies now. */
  static void begin(uint64_t seq);
  /** The request ended; whether it was interrupted. */
  static bool end();
};

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_MAILBOX_HPP
