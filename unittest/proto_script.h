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

#ifndef MLE_PROTO_SCRIPT_H
#define MLE_PROTO_SCRIPT_H

// What mle_test_worker does in its "proto" mode is told by scripts that the
// test writes: a list of steps, each a post or a pause. A script reaches the
// worker
//   - as argv[2] (hex), run instead of the READY of a good worker;
//   - in a PREPARE's source, "prepare:<hex>" (run when that PREPARE arrives)
//     or "call:<hex>" (run at every CALL of the routine);
//   - in a PARSE_LIBRARY's body, "script:<hex>";
//   - through an arm step of an earlier script, for the next DROP, RESET or
//     OPTIONS, or for the next CONTINUE the worker owes.
// Without a script the worker answers as a conforming worker would.

#include <cstdint>
#include <string>
#include <string_view>

#include "mle/codec.h"
#include "mle/protocol.h"

namespace mle_test {

enum class Step : uint8_t {
  /** One post, exactly as given. */
  kPost = 1,
  /** Waits for the host's next post and leaves it unread. */
  kWait,
  /** A well-formed message, in chunks if needed. After anything but a
      RESULT the worker receives the host's reply, serving the nested
      requests that come before it. */
  kMessage,
  kSleep,
  /** Blocks until the INTERRUPT for the request arrives. */
  kAwaitInterrupt,
  kExit,
  kTainted,
  /** Writes impossible statistics. */
  kBadStats,
  /** Keeps a script for the next message of an op (see above). */
  kArm,
  /** Tries to shrink the mailbox; exits with 8 if that works. */
  kResize,
  /** The RESULT of a conforming worker; its stdout is the log, if any. */
  kAnswer,
  /** Runs a script a number of times. */
  kLoop,
  /** Appends the request (header and body) to the log, and from now on
      every reply the worker receives. */
  kLog,
};

/** In a kPost: the value the worker computes itself. */
constexpr uint32_t kAuto = 0xFFFFFFFE;

class Script {
 public:
  /** A raw post. depth and call_seq are those of the request plus the
      deltas. The arena gets arena (cut to its size); len and total_len are
      what the header says, by default the size of arena. */
  Script &post(uint32_t op, uint32_t status, uint32_t flags,
               std::string_view arena, uint32_t len = kAuto,
               uint64_t total_len = kAuto, int32_t depth_delta = 0,
               int64_t seq_delta = 0, uint32_t reserved = 0) {
    step(Step::kPost);
    w().u32(op);
    w().u32(status);
    w().u32(flags);
    w().u32(len);
    w().u64(total_len);
    w().u32(static_cast<uint32_t>(depth_delta));
    w().i64(seq_delta);
    w().u32(reserved);
    w().str(arena);
    return *this;
  }
  Script &wait() { return step(Step::kWait); }
  /** chunk_delay_ms: a pause before every chunk but the first. */
  Script &message(mle::wire::Op op, uint32_t status, std::string_view body,
                  uint32_t chunk_delay_ms = 0) {
    step(Step::kMessage);
    w().u32(static_cast<uint32_t>(op));
    w().u32(status);
    w().u32(chunk_delay_ms);
    w().str(body);
    return *this;
  }
  template <class M>
  Script &message(mle::wire::Op op, const M &m, uint32_t status = 0,
                  uint32_t chunk_delay_ms = 0) {
    std::string body;
    mle::encode(m, &body);
    return message(op, status, body, chunk_delay_ms);
  }
  Script &result(const mle::ResultMsg &m, uint32_t status = 0,
                 uint32_t chunk_delay_ms = 0) {
    return message(mle::wire::Op::kResult, m, status, chunk_delay_ms);
  }
  Script &sleep(uint32_t ms) {
    step(Step::kSleep);
    w().u32(ms);
    return *this;
  }
  Script &await_interrupt() { return step(Step::kAwaitInterrupt); }
  Script &exit(uint32_t status) {
    step(Step::kExit);
    w().u32(status);
    return *this;
  }
  Script &tainted() { return step(Step::kTainted); }
  Script &bad_stats() { return step(Step::kBadStats); }
  Script &arm(mle::wire::Op op, const Script &script) {
    step(Step::kArm);
    w().u32(static_cast<uint32_t>(op));
    w().str(script.m_bytes);
    return *this;
  }
  Script &resize() { return step(Step::kResize); }
  Script &answer() { return step(Step::kAnswer); }
  Script &loop(uint32_t count, const Script &script) {
    step(Step::kLoop);
    w().u32(count);
    w().str(script.m_bytes);
    return *this;
  }
  Script &log() { return step(Step::kLog); }

  const std::string &bytes() const { return m_bytes; }
  std::string hex() const {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * m_bytes.size());
    for (const unsigned char c : m_bytes) {
      out += kDigits[c >> 4];
      out += kDigits[c & 15];
    }
    return out;
  }

 private:
  Script &step(Step s) {
    w().u8(static_cast<uint8_t>(s));
    return *this;
  }
  mle::Writer w() { return mle::Writer(&m_bytes); }
  std::string m_bytes;
};

inline std::string unhex(std::string_view hex) {
  auto digit = [](char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out += static_cast<char>(digit(hex[i]) << 4 | digit(hex[i + 1]));
  return out;
}

}  // namespace mle_test

#endif  // MLE_PROTO_SCRIPT_H
