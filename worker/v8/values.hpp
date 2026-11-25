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

  MySQL values and JavaScript values in the V8 worker, by Oracle's rules as the JVM worker's GraalJS
  has them (JsOracle.java): wire values (docs/protocol.md §7.2) of arguments, SQL rows and
  parameters to V8 values, and V8 results to wire values of the declared SQL type. Of the host's
  values the lengths are checked (§1) and each is decoded by its own tag: a value outside its
  domain gives guest code an unspecified value (an Invalid Date, a number). What guest code gives outside the wire's
  domains is a ConversionError, so that no value of guest code breaks the protocol.
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

using common::protocol_failure;
using common::Reader;
using common::Writer;

/** A value that cannot become the SQL type: the routine's error "MLE-Type> message". */
struct ConversionError {
  std::string message;
};

/** Guest code that a conversion runs (toString, toJSON, valueOf, a getter) threw. The exception is
    still pending in V8: it becomes the routine's error, or the SQL API call's exception. */
struct GuestThrew {};

/** The message being written would exceed the limit of a worker's message (64 MiB, §12). */
struct TooLarge {};

/** A parameter's, result's or column's SQL type (§7.4: Type). */
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

/** The session option integer_type (Appendix B.2): how JavaScript receives I64 and U64. The values
    are also sql.js's codes of a statement's integerType. */
enum class IntegerType : uint8_t { kUnsafeString = 0, kUnsafeBigint = 1, kString = 2, kBigint = 3 };
constexpr int kMaxIntegerType = 3;
/** The session option decimal_type: how JavaScript receives DECIMAL. */
enum class DecimalType : uint8_t { kString = 0, kNumber = 1 };
constexpr int kMaxDecimalType = 1;

/** The options in force (OPTIONS, §9.8): the session options JavaScript uses and the session's
    time zone, in which DATE and DATETIME values are Dates. */
struct Options {
  IntegerType integer_type = IntegerType::kUnsafeString;
  DecimalType decimal_type = DecimalType::kString;
  /** The zone (nullptr: a fixed offset). Without OPTIONS: +00:00 (§1). */
  const std::chrono::time_zone *zone = nullptr;
  std::chrono::seconds offset{0};

  /** An IANA name or an offset +hh:mm / -hh:mm from -13:59 to +14:00; false if it is neither. */
  bool set_zone(std::string_view text);
  /** Milliseconds since the epoch of a local date-time in the zone (Java's atZone: a gap moves
      forward, an overlap takes the earlier offset). month and day are at least 1; fields past
      their ranges roll over, as the Date constructor's do. */
  double epoch_ms(int year, unsigned month, unsigned day, int64_t hour, unsigned minute, unsigned second,
                  unsigned micro) const;
  /** The local date-time in the zone of milliseconds since the epoch. */
  void local(double ms, int *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute,
             unsigned *second, unsigned *micro) const;

 private:
  /** The interval of one offset of the zone, in seconds since the epoch (instants) or of local
      time: a value inside it costs a compare and an add, only one outside looks the zone up
      (Appendix A). The local interval leaves out the local times its bounding transitions repeat
      or skip. Empty: begin > end. */
  struct Interval {
    int64_t begin = 1, end = 0, offset = 0;
    bool holds(int64_t t) const { return t >= begin && t < end; }
  };
  mutable Interval m_instants, m_locals;
};

/** A Civil value as on the wire (§7.2): a DATE, TIME or DATETIME. */
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
  /** Made before guest code runs: it keeps the context's Date constructor as it is then. */
  Values(v8::Isolate *isolate, v8::Local<v8::Context> context, const Options *options);

  /** A routine argument of type t from the wire (Oracle's conversions, as JsOracle.hostForm and
      its converters do them): DATE, DATETIME and TIMESTAMP as a Date, a zero or partial date and
      the zero TIMESTAMP as an Invalid Date; TIME as a string; a SET as a Set. */
  v8::Local<v8::Value> argument(Reader &r, const ParamType &t);

  /** A value of an SQL row under its column's type t (Oracle's SQL API rules: as an argument,
      but a SET stays a string).

      argument() and column() decode and convert the value by its own tag (§1 item 2), and do not
      compare it with t's: t only refines it (YEAR, BIT, SET). They throw ConversionError for a
      value JavaScript cannot hold, and only after they have read the value completely: the
      caller can go on reading the message. */
  v8::Local<v8::Value> column(Reader &r, const ParamType &t);

  /** While it lives, column() uses a statement's integerType and decimalType, not the session's. */
  class Rules {
   public:
    Rules(Values &values, const Options *rules) : m_slot(values.m_rules), m_saved(values.m_rules) { m_slot = rules; }
    ~Rules() { m_slot = m_saved; }
    Rules(const Rules &) = delete;
    Rules &operator=(const Rules &) = delete;

   private:
    const Options *&m_slot;
    const Options *m_saved;
  };

  /** Writes result v as type t (JsOracle.toJava, then Values.writeAs); ConversionError if it
      cannot be one, GuestThrew if guest code it runs throws, TooLarge if the message would exceed
      its limit. */
  void write_as(Writer &w, v8::Local<v8::Value> v, const ParamType &t);

  /** Writes v as a SQL parameter (its own JavaScript type decides; Oracle's binding rules).
      ConversionError, GuestThrew and TooLarge as write_as. */
  void write_param(Writer &w, v8::Local<v8::Value> v);

  /** String(v) for a message: "?" if that throws. */
  std::string str(v8::Local<v8::Value> v);

  /** String(v) for a value: a symbol is "Symbol(description)"; GuestThrew if guest code throws. */
  std::string text(v8::Local<v8::Value> v);
  v8::Local<v8::String> string_of(v8::Local<v8::Value> v);

  v8::Isolate *isolate() const { return m_iso; }
  v8::Local<v8::Context> context() const { return m_ctx.Get(m_iso); }

 private:
  v8::Local<v8::Value> integer(int64_t v, bool is_unsigned, const ParamType *t);
  v8::Local<v8::Value> value(Reader &r, const ParamType &t, bool argument);
  v8::Local<v8::Value> temporal(const Temporal &t);
  [[noreturn]] void cannot(v8::Local<v8::Value> v, const ParamType &t);
  /** Oracle's Table 27.3 for DATE, DATETIME and TIMESTAMP: a Date's time value, or a string cast
      to a Date (MySQL's literal forms as local time, as JsOracle's date_ms); NaN for anything else. */
  double date_ms(v8::Local<v8::Value> v);
  /** Table 27.3 for YEAR: a Date's local year, a string's 4-digit year; nullopt: the integer rules. */
  std::optional<double> year_of(v8::Local<v8::Value> v);
  /** The JSON text of v by Oracle's JavaScript-to-JSON table (JsOracle's json_text); nullopt if v
      has none. */
  std::optional<std::string> json_text(v8::Local<v8::Value> v);

  const Options *rules() const { return m_rules != nullptr ? m_rules : m_opt; }

  v8::Isolate *m_iso;
  v8::Global<v8::Context> m_ctx;
  v8::Global<v8::Function> m_date;  // Date, as it was before guest code ran
  const Options *m_opt;
  const Options *m_rules = nullptr;  // a statement's, while its rows are decoded
};

/** A UTF-8 std::string of a V8 string (or of String(v)), repaired (§7.1): each unpaired surrogate
    becomes U+FFFD. For data and messages, never for code or names. One pass over the string. */
std::string utf8(v8::Isolate *iso, v8::Local<v8::Value> v);
/** Writes s as a str, repaired as utf8() does, in one pass into the message; false if it does not
    fit. */
bool write_utf8(v8::Isolate *iso, Writer &w, v8::Local<v8::String> s);
/** The UTF-8 of a V8 string that is code or a name (SQL text, a library's name), which is never
    repaired; nullopt if it has an unpaired surrogate. */
std::optional<std::string> utf8_exact(v8::Isolate *iso, v8::Local<v8::String> s);
/** A V8 string of UTF-8 text (internalized when short); ill-formed sequences are repaired as §7.1
    says (U+FFFD per maximal subpart). ConversionError if it is longer than V8's strings can be. */
v8::Local<v8::String> v8_string(v8::Isolate *iso, std::string_view s);

/** The domain of a Civil value under its tag (§7.2). */
bool valid_temporal(const Temporal &t);
/** The domain of a DECIMAL's text (§7.2). */
bool valid_decimal(std::string_view s);
/** Reads a Civil (after its tag) as it is: its domain is the host's to keep (§1). */
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
