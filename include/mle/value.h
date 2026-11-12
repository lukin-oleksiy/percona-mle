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

#ifndef MLE_VALUE_H
#define MLE_VALUE_H

/**
  @file include/mle/value.h

  A typed SQL value as carried by the MLE wire protocol (protocol.h, Tag).
  Owns its bytes. No MySQL types: the conversion from and to MySQL values is
  done by the component.
*/

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mle/protocol.h"

namespace mle {

using wire::Tag;

/** The fields of MYSQL_TIME that the protocol carries. DATE uses only the
    date part, TIME only hour (may exceed 23), minute, second, microsecond
    and negative. */
struct Temporal {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint32_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint32_t microsecond = 0;
  bool negative = false;

  bool operator==(const Temporal &) const = default;
};

class Value {
 public:
  Value() = default;

  static Value null() { return {}; }
  static Value i64(int64_t v) { return {Tag::kI64, v}; }
  static Value u64(uint64_t v) { return {Tag::kU64, v}; }
  static Value f64(double v) { return {Tag::kF64, v}; }
  static Value f32(float v) { return {Tag::kF32, v}; }
  static Value decimal(std::string text) {
    return {Tag::kDecimal, std::move(text)};
  }
  static Value string(std::string utf8) {
    return {Tag::kString, std::move(utf8)};
  }
  static Value bytes(std::string data) {
    return {Tag::kBytes, std::move(data)};
  }
  static Value json(std::string text) { return {Tag::kJson, std::move(text)}; }
  /** tag is one of kDate, kTime, kDatetime, kTimestamp. */
  static Value temporal(Tag tag, const Temporal &t) { return {tag, t}; }
  static Value vector(std::vector<float> v) {
    return {Tag::kVector, std::move(v)};
  }

  Tag tag() const { return m_tag; }
  bool is_null() const { return m_tag == Tag::kNull; }

  /** Accessors; the tag must match (checked by std::get). */
  int64_t as_i64() const { return std::get<int64_t>(m_data); }
  uint64_t as_u64() const { return std::get<uint64_t>(m_data); }
  double as_f64() const { return std::get<double>(m_data); }
  float as_f32() const { return std::get<float>(m_data); }
  /** kDecimal, kString, kBytes, kJson. */
  const std::string &as_text() const { return std::get<std::string>(m_data); }
  const Temporal &as_temporal() const { return std::get<Temporal>(m_data); }
  const std::vector<float> &as_vector() const {
    return std::get<std::vector<float>>(m_data);
  }

  /** Exact equality: floating point values compare by bit pattern, so a NaN
      equals the same NaN and 0.0 differs from -0.0. */
  bool operator==(const Value &other) const;

 private:
  using Data = std::variant<std::monostate, int64_t, uint64_t, double, float,
                            std::string, Temporal, std::vector<float>>;
  template <class T>
  Value(Tag tag, T &&v)
      : m_tag(tag),
        m_data(std::in_place_type<std::decay_t<T>>, std::forward<T>(v)) {}

  Tag m_tag = Tag::kNull;
  Data m_data;
};

/** Name of a tag for messages, e.g. "VECTOR". */
const char *tag_name(Tag tag);

}  // namespace mle

#endif  // MLE_VALUE_H
