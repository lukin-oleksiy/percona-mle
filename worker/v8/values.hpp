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
  @file worker/v8/values.hpp

  MySQL values and JavaScript values in the V8 worker (exp design note 63), by Oracle's rules as the
  JVM worker's GraalJS has them (JsOracle.java): wire values of arguments, SQL rows and parameters
  to V8 values, and V8 results to wire values of the declared SQL type.
*/

#ifndef MLE_V8_VALUES_HPP
#define MLE_V8_VALUES_HPP

#include <v8.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/wire.hpp"
#include "mle/protocol.h"

namespace mle::v8w {

using common::Malformed;
using common::Reader;
using common::Writer;

/** A value that cannot become the SQL type: the routine's error "MLE-Type> message". */
struct ConversionError {
  std::string message;
};

/** The arena is full (the RESULT goes in chunks, through a larger buffer). */
struct ArenaFull {};

/** A parameter's or result's SQL type (protocol.h: ParamType). */
struct ParamType {
  wire::Tag tag = wire::Tag::kNull;
  bool is_unsigned = false;
  uint32_t length = 0, precision = 0, scale = 0;
  std::string type_name;

  static ParamType read(Reader &r);
  /** The name in messages: the SQL type name, else the tag's (Wire.TAG_NAMES). */
  std::string sql_name() const;
  bool is(std::string_view name) const { return type_name == name; }
};

/** The session's options (OPTIONS): Oracle's integer_type and decimal_type, and the time zone of
    DATE and DATETIME values. */
struct Options {
  wire::IntegerType integer_type = wire::IntegerType::kUnsafeString;
  wire::DecimalType decimal_type = wire::DecimalType::kString;
  /** The zone (nullptr: a fixed offset). */
  const std::chrono::time_zone *zone = nullptr;
  std::chrono::seconds offset{0};

  /** An IANA name or +hh:mm / -hh:mm; false if unknown. */
  bool set_zone(std::string_view text);
  /** Milliseconds since the epoch of a local date-time in the zone (Java's atZone: a gap moves
      forward, an overlap takes the earlier offset). */
  double epoch_ms(int year, unsigned month, unsigned day, int64_t hour, unsigned minute, unsigned second,
                  unsigned micro) const;
  /** The local date-time in the zone of milliseconds since the epoch. */
  void local(double ms, int *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute,
             unsigned *second, unsigned *micro) const;
};

/** A temporal value as on the wire. */
struct Temporal {
  wire::Tag tag;
  uint16_t year;
  uint8_t month, day;
  uint32_t hour;
  uint8_t minute, second;
  uint32_t micro;
  bool negative;
};

/** What the conversions need of the context: the session options and JSON. */
class Values {
 public:
  Values(v8::Isolate *isolate, v8::Local<v8::Context> context, const Options *options)
      : m_iso(isolate), m_ctx(isolate, context), m_opt(options) {}

  /** A routine argument of type t from the wire (Oracle's conversions, as JsOracle.hostForm and
      its converters do them). */
  v8::Local<v8::Value> argument(Reader &r, const ParamType &t);

  /** A SQL value (a row's column, an OUT value) without a declared parameter type: the value's
      own tag decides (Oracle's SQL API rules). type_name: the column's SQL type ("SET", "BIT",
      "YEAR"; empty if unknown). rules: the statement's integerType and decimalType (nullptr: the
      session's). */
  v8::Local<v8::Value> column(Reader &r, std::string_view type_name, const Options *rules = nullptr);

  /** Writes result v as type t (JsOracle.toJava, then Values.writeAs); ConversionError if it
      cannot be one, ArenaFull if it does not fit. */
  void write_as(Writer &w, v8::Local<v8::Value> v, const ParamType &t);

  /** Writes v as a SQL parameter (its own JavaScript type decides; Oracle's binding rules). */
  void write_param(Writer &w, v8::Local<v8::Value> v);

  /** String(v), or "?" if that throws. */
  std::string str(v8::Local<v8::Value> v);

  v8::Isolate *isolate() const { return m_iso; }
  v8::Local<v8::Context> context() const { return m_ctx.Get(m_iso); }

 private:
  v8::Local<v8::Value> integer(int64_t v, bool is_unsigned, const ParamType *t);
  v8::Local<v8::Value> temporal(const Temporal &t, bool as_date);
  [[noreturn]] void cannot(v8::Local<v8::Value> v, const ParamType &t);

  const Options *rules() const { return m_rules != nullptr ? m_rules : m_opt; }

  v8::Isolate *m_iso;
  v8::Global<v8::Context> m_ctx;
  const Options *m_opt;
  const Options *m_rules = nullptr;  // a statement's, while its rows are decoded
};

/** A UTF-8 std::string of a V8 string (or of String(v)). */
std::string utf8(v8::Isolate *iso, v8::Local<v8::Value> v);
/** A V8 string of UTF-8 text (internalized when short). */
v8::Local<v8::String> v8_string(v8::Isolate *iso, std::string_view s);

/** Reads a Temporal (after its tag). */
Temporal read_temporal(Reader &r, wire::Tag tag);
void write_temporal(Writer &w, const Temporal &t);
/** "[-]hh:mm:ss[.ffffff]" of a TIME. */
std::string time_text(const Temporal &t);
/** A TIME from [-]h:mm:ss[.ffffff]; nullopt if it is not one. */
std::optional<Temporal> parse_time(std::string_view s);

/** A decimal number's text (JavaScript's String(n): plain or with an exponent, or a DECIMAL's)
    as DECIMAL's plain text within MySQL's limits (65 digits, 30 after the point); nullopt if it is
    not a number, ConversionError if out of range. target names the type in the message. */
std::optional<std::string> decimal_text(std::string_view s, std::string_view target);

}  // namespace mle::v8w

#endif  // MLE_V8_VALUES_HPP
