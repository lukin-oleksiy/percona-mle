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
  @file worker/common/wire.hpp

  Reading and writing protocol messages (docs/protocol.md §7, include/mle/protocol.h) in the native
  workers (CPython, V8): bounds-checked, over spans of the arena or of a reassembled message.
*/

#ifndef MLE_WORKER_COMMON_WIRE_HPP
#define MLE_WORKER_COMMON_WIRE_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "mle/protocol.h"

namespace mle::common {

static_assert(std::endian::native == std::endian::little, "the wire is little-endian");

/** A protocol failure (§1): the host sent something that breaks the protocol, or the worker's own
    code would. Writes what to stderr and ends the process with a non-zero status at once: no
    exception, no unwinding, no guest code. The one path for every such failure (mailbox.cc). */
[[noreturn]] void protocol_failure(std::string_view what);

/** Reads a message body of the host with the checks of §1: every length and count against the
    bytes the message has. Reading beyond the message is a protocol failure; domains, reserved
    fields and trailing bytes are the host's to get right and are not looked at. */
class Reader {
 public:
  explicit Reader(std::span<const std::byte> b, size_t pos = 0) : m_b(b), m_p(pos) {
    if (pos > b.size()) protocol_failure("position beyond the message");
  }
  template <class T>
  T get() {
    std::array<std::byte, sizeof(T)> a;
    std::memcpy(a.data(), take(sizeof(T)).data(), sizeof(T));
    return std::bit_cast<T>(a);
  }
  std::span<const std::byte> take(size_t n) {
    if (n > m_b.size() - m_p) protocol_failure("message too short");
    auto s = m_b.subspan(m_p, n);
    m_p += n;
    return s;
  }
  /** A str, an lstr or bytes: the bytes as they are. */
  std::string_view str() {
    const auto n = get<uint32_t>();
    auto s = take(n);
    return {reinterpret_cast<const char *>(s.data()), s.size()};
  }
  bool boolean() { return get<uint8_t>() != 0; }
  /** The count of a list of at most max elements of at least min_size bytes each (1 or more),
      checked against the bytes left before anything is allocated for it (§1 item 1, §7.1). */
  uint32_t count(uint32_t max, size_t min_size) {
    const auto n = get<uint32_t>();
    if (n > max || n > left() / min_size) protocol_failure("a list longer than the message or its limit");
    return n;
  }
  /** Skips a VECTOR's padding: up to kVectorAlignment, counted from the body's start. */
  void align() { take((wire::kVectorAlignment - m_p % wire::kVectorAlignment) % wire::kVectorAlignment); }
  /** For a caller that wants the body consumed exactly; a worker need not look (§5.1). */
  void finish() const {
    if (m_p != m_b.size()) protocol_failure("trailing bytes in a message");
  }
  size_t pos() const { return m_p; }
  size_t left() const { return m_b.size() - m_p; }
  std::span<const std::byte> all() const { return m_b; }

 private:
  std::span<const std::byte> m_b;
  size_t m_p;
};

/** Writes a message body: into a fixed span (the arena), or into a buffer that grows up to a limit.
    put() and the others return false when the span is full or the limit would be exceeded; what
    was written before stays. */
class Writer {
 public:
  explicit Writer(std::span<std::byte> b) : m_b(b) {}
  /** Into buffer, from its start; it grows as needed, to at most limit bytes. written() is the
      message (the buffer may be larger). */
  Writer(std::vector<std::byte> &buffer, size_t limit) : m_b(buffer), m_grow(&buffer), m_limit(limit) {}
  template <class T>
  bool put(T v) {
    const auto a = std::bit_cast<std::array<std::byte, sizeof(T)>>(v);
    return bytes(a);
  }
  bool bytes(std::span<const std::byte> s) {
    if (s.size() > m_b.size() - m_p && !grow(s.size())) return false;
    if (!s.empty()) std::memcpy(m_b.data() + m_p, s.data(), s.size());
    m_p += s.size();
    return true;
  }
  bool str(std::string_view s) {
    return s.size() <= UINT32_MAX && put(static_cast<uint32_t>(s.size())) &&
           bytes(std::as_bytes(std::span(s.data(), s.size())));
  }
  /** A str whose bytes fill(span) writes in place, at most max of them; fill returns how many it
      wrote. For text whose encoded length is known only once it is encoded: one pass (§7.1).
      False, and nothing written, if max bytes do not fit. */
  template <class F>
  bool str_in_place(size_t max, F &&fill) {
    const size_t at = m_p;
    if (max > UINT32_MAX || !put<uint32_t>(0)) return false;
    if (max > m_b.size() - m_p && !grow(max)) {
      m_p = at;
      return false;
    }
    const size_t n = std::min<size_t>(fill(m_b.subspan(m_p, max)), max);
    const auto len = static_cast<uint32_t>(n);
    std::memcpy(m_b.data() + at, &len, sizeof len);
    m_p += n;
    return true;
  }
  /** Zero bytes up to the next kVectorAlignment (relative to the message start). */
  bool align() {
    static constexpr std::array<std::byte, wire::kVectorAlignment> kZero{};
    return bytes(std::span(kZero).first((wire::kVectorAlignment - m_p % wire::kVectorAlignment) %
                                        wire::kVectorAlignment));
  }
  size_t size() const { return m_p; }
  std::span<std::byte> written() const { return m_b.first(m_p); }

 private:
  bool grow(size_t n) {
    if (m_grow == nullptr || n > m_limit - m_p) return false;
    const size_t need = m_p + n;
    m_grow->resize(std::min(m_limit, std::max({need, m_b.size() * 2, size_t{4096}})));
    m_b = *m_grow;
    return true;
  }

  std::span<std::byte> m_b;
  std::vector<std::byte> *m_grow = nullptr;
  size_t m_limit = 0;
  size_t m_p = 0;
};

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_WIRE_HPP
