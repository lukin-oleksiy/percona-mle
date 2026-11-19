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

  Reading and writing protocol messages (include/mle/protocol.h) in the native workers (CPython,
  V8): bounds-checked, over spans of the arena or of a reassembled message (exp design note 63).
*/

#ifndef MLE_WORKER_COMMON_WIRE_HPP
#define MLE_WORKER_COMMON_WIRE_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "mle/protocol.h"

namespace mle::common {

static_assert(std::endian::native == std::endian::little, "the wire is little-endian");

/** A message that does not follow the protocol. */
struct Malformed {
  const char *what;
};

class Reader {
 public:
  Reader(std::span<const std::byte> b, size_t pos) : m_b(b), m_p(pos) {
    if (pos > b.size()) throw Malformed{"position beyond the message"};
  }
  template <class T>
  T get() {
    std::array<std::byte, sizeof(T)> a;
    std::memcpy(a.data(), take(sizeof(T)).data(), sizeof(T));
    return std::bit_cast<T>(a);
  }
  std::span<const std::byte> take(size_t n) {
    if (n > m_b.size() - m_p) throw Malformed{"message too short"};
    auto s = m_b.subspan(m_p, n);
    m_p += n;
    return s;
  }
  std::string_view str() {
    const auto n = get<uint32_t>();
    auto s = take(n);
    return {reinterpret_cast<const char *>(s.data()), s.size()};
  }
  /** VECTOR data starts at kVectorAlignment relative to the message start. */
  void align() { take((wire::kVectorAlignment - m_p % wire::kVectorAlignment) % wire::kVectorAlignment); }
  size_t pos() const { return m_p; }
  size_t left() const { return m_b.size() - m_p; }
  std::span<const std::byte> all() const { return m_b; }

 private:
  std::span<const std::byte> m_b;
  size_t m_p;
};

/** Writes into a fixed span (the arena); false from put() and the others when it is full. */
class Writer {
 public:
  explicit Writer(std::span<std::byte> b) : m_b(b) {}
  template <class T>
  bool put(T v) {
    const auto a = std::bit_cast<std::array<std::byte, sizeof(T)>>(v);
    return bytes(a);
  }
  bool bytes(std::span<const std::byte> s) {
    if (s.size() > m_b.size() - m_p) return false;
    std::memcpy(m_b.data() + m_p, s.data(), s.size());
    m_p += s.size();
    return true;
  }
  bool str(std::string_view s) {
    return put(static_cast<uint32_t>(s.size())) && bytes(std::as_bytes(std::span(s.data(), s.size())));
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
  std::span<std::byte> m_b;
  size_t m_p = 0;
};

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_WIRE_HPP
