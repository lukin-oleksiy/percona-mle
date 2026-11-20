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

// The V8 worker's values (worker/v8/values.cc): DECIMAL text within MySQL's limits, TIME text,
// time zones, arguments to JavaScript and results back by Oracle's rules (as the JVM worker's
// JsOracle), the domains of docs/protocol.md §7.2 in both directions, text that is not well-formed,
// and malformed messages, which are protocol failures. In an isolate of the distribution's V8.

#include <gtest/gtest.h>
#include <libplatform/libplatform.h>
#include <v8.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "values.hpp"

/** What ends the worker (worker/common/mailbox.cc) is an exception here, so that a test can see it. */
struct ProtocolFailure {
  std::string what;
};
void mle::common::protocol_failure(std::string_view what) { throw ProtocolFailure{std::string(what)}; }

namespace {

using mle::v8w::ConversionError;
using mle::v8w::decimal_text;
using mle::v8w::Options;
using mle::v8w::ParamType;
using mle::v8w::parse_time;
using mle::v8w::Reader;
using mle::v8w::Values;
using mle::v8w::Writer;
namespace w = mle::wire;

class V8Values : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    s_platform = v8::platform::NewDefaultPlatform(1).release();
    v8::V8::InitializeICU();
    v8::V8::InitializePlatform(s_platform);
    v8::V8::Initialize();
  }

  void SetUp() override {
    m_allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams p;
    p.array_buffer_allocator = m_allocator.get();
    m_iso = v8::Isolate::New(p);
    m_iso->Enter();
    m_scope.emplace(m_iso);
    m_ctx = v8::Context::New(m_iso);
    m_ctx->Enter();
    ASSERT_TRUE(m_opt.set_zone("Europe/Kyiv"));
    m_values = std::make_unique<Values>(m_iso, m_ctx, &m_opt);
  }

  void TearDown() override {
    m_values.reset();
    m_ctx->Exit();
    m_scope.reset();
    m_iso->Exit();
    m_iso->Dispose();
  }

  v8::Local<v8::Value> js(const char *src) {
    return v8::Script::Compile(m_ctx, v8::String::NewFromUtf8(m_iso, src).ToLocalChecked())
        .ToLocalChecked()
        ->Run(m_ctx)
        .ToLocalChecked();
  }
  std::string str(v8::Local<v8::Value> v) { return m_values->str(v); }

  /** v written as type t, then read back as a column. */
  std::string round_trip(const char *src, ParamType t) {
    std::array<std::byte, 4096> buf{};
    Writer wr(buf);
    m_values->write_as(wr, js(src), t);
    Reader r(std::span<const std::byte>(buf.data(), wr.size()), 0);
    const v8::Local<v8::Value> back = m_values->column(r, t);
    r.finish();
    return str(back);
  }

  /** The bytes of a value written as type t. */
  std::vector<std::byte> written(const char *src, ParamType t) {
    std::array<std::byte, 4096> buf{};
    Writer wr(buf);
    m_values->write_as(wr, js(src), t);
    return {buf.begin(), buf.begin() + wr.size()};
  }

  template <class... T>
  static std::vector<std::byte> bytes(T... b) {
    return {static_cast<std::byte>(b)...};
  }
  /** A message of a tag and a str. */
  static std::vector<std::byte> text_value(w::Tag tag, std::string_view text) {
    std::vector<std::byte> out(5 + text.size());
    Writer wr(out);
    EXPECT_TRUE(wr.put(static_cast<uint8_t>(tag)) && wr.str(text));
    return out;
  }

  static ParamType type(w::Tag tag, const char *name = "", bool is_unsigned = false) {
    ParamType t;
    t.tag = tag;
    t.type_name = name;
    t.is_unsigned = is_unsigned;
    return t;
  }

  static inline v8::Platform *s_platform = nullptr;
  std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
  v8::Isolate *m_iso = nullptr;
  std::optional<v8::HandleScope> m_scope;
  v8::Local<v8::Context> m_ctx;
  Options m_opt;
  std::unique_ptr<Values> m_values;
};

TEST_F(V8Values, DecimalText) {
  EXPECT_EQ(decimal_text("1e21", "DECIMAL").value(), "1000000000000000000000");
  EXPECT_EQ(decimal_text(" -0.50 ", "DECIMAL").value(), "-0.50");
  EXPECT_EQ(decimal_text("-0.0", "DECIMAL").value(), "0.0");
  EXPECT_EQ(decimal_text("1.5e-40", "DECIMAL").value(), "0");
  // more than 30 digits after the point: rounded half up
  EXPECT_EQ(decimal_text("0.1234567890123456789012345678905", "DECIMAL").value(),
            "0.123456789012345678901234567891");
  EXPECT_FALSE(decimal_text("12abc", "DECIMAL").has_value());
  EXPECT_FALSE(decimal_text("0x10", "DECIMAL").has_value());
  EXPECT_THROW(decimal_text("1e70", "DECIMAL(10,2)"), ConversionError);
}

TEST_F(V8Values, TimeText) {
  auto t = parse_time("-838:59:59.000000");
  ASSERT_TRUE(t.has_value());
  EXPECT_TRUE(t->negative);
  EXPECT_EQ(t->hour, 838u);
  EXPECT_EQ(mle::v8w::time_text(*parse_time("1:02:03.5")), "01:02:03.500000");
  EXPECT_FALSE(parse_time("10:60:00").has_value());
  EXPECT_FALSE(parse_time("10:00").has_value());
  EXPECT_FALSE(parse_time("12345:00:00").has_value());
}

TEST_F(V8Values, Zones) {
  Options o;
  EXPECT_TRUE(o.set_zone("+05:30"));
  EXPECT_EQ(o.epoch_ms(1970, 1, 1, 5, 30, 0, 0), 0);
  EXPECT_FALSE(o.set_zone("Mars/Olympus"));
  // offsets from -13:59 to +14:00, in the form +hh:mm only
  EXPECT_TRUE(o.set_zone("+14:00"));
  EXPECT_TRUE(o.set_zone("-13:59"));
  EXPECT_EQ(o.epoch_ms(1970, 1, 1, 0, 0, 0, 0), (13 * 60 + 59) * 60000.0);
  for (const char *bad : {"+14:01", "-14:00", "+5:30", "+05:60", "+0530", "+05:30x", "SYSTEM", ""})
    EXPECT_FALSE(o.set_zone(bad)) << bad;
  EXPECT_TRUE(o.set_zone("Europe/Kyiv"));
  // a local time in the spring gap moves forward (Java's atZone): 03:30 -> 04:30 EEST
  const double gap = o.epoch_ms(2026, 3, 29, 3, 30, 0, 0);
  int y;
  unsigned mo, d, h, mi, s, us;
  o.local(gap, &y, &mo, &d, &h, &mi, &s, &us);
  EXPECT_EQ(h, 4u);
  EXPECT_EQ(mi, 30u);
}

TEST_F(V8Values, Integers) {
  // BIGINT within 2^53: a number; beyond: a string (UNSAFE_STRING) or a BigInt (UNSAFE_BIGINT)
  EXPECT_EQ(round_trip("9007199254740991", type(w::Tag::kI64, "BIGINT")), "9007199254740991");
  EXPECT_EQ(round_trip("9007199254740993n", type(w::Tag::kI64, "BIGINT")), "9007199254740993");
  EXPECT_EQ(round_trip("2.5", type(w::Tag::kI64, "INT")), "3");     // Math.round
  EXPECT_EQ(round_trip("'  42 '", type(w::Tag::kI64, "INT")), "42");
  EXPECT_EQ(round_trip("true", type(w::Tag::kI64, "TINYINT")), "1");
  EXPECT_THROW(round_trip("128", type(w::Tag::kI64, "TINYINT")), ConversionError);
  EXPECT_THROW(round_trip("-1", type(w::Tag::kU64, "INT", true)), ConversionError);
  EXPECT_THROW(round_trip("'abc'", type(w::Tag::kI64, "INT")), ConversionError);
  EXPECT_THROW(round_trip("NaN", type(w::Tag::kI64, "INT")), ConversionError);
  m_opt.integer_type = mle::v8w::IntegerType::kUnsafeBigint;
  std::array<std::byte, 16> buf{};
  Writer wr(buf);
  ASSERT_TRUE(wr.put(uint8_t(w::Tag::kI64)) && wr.put(int64_t{1} << 60));
  Reader r(std::span<const std::byte>(buf.data(), wr.size()), 0);
  EXPECT_TRUE(m_values->column(r, type(w::Tag::kI64, "BIGINT"))->IsBigInt());
}

TEST_F(V8Values, Others) {
  EXPECT_EQ(round_trip("({a: [1, 'b']})", type(w::Tag::kJson)), "[object Object]");
  EXPECT_THROW(round_trip("42", type(w::Tag::kJson)), ConversionError);       // not a JSON document
  EXPECT_THROW(round_trip("'{bad'", type(w::Tag::kJson)), ConversionError);   // not JSON text
  EXPECT_EQ(round_trip("new Set(['a', 'b'])", type(w::Tag::kString, "SET")), "a,b");
  EXPECT_EQ(round_trip("new Float32Array([1, 2.5])", type(w::Tag::kVector)), "1,2.5");
  EXPECT_EQ(round_trip("[3, 4]", type(w::Tag::kVector)), "3,4");
  EXPECT_EQ(round_trip("new Uint8Array([104, 105])", type(w::Tag::kBytes)), "104,105");
  EXPECT_THROW(round_trip("'text'", type(w::Tag::kBytes)), ConversionError);
  EXPECT_EQ(round_trip("'-01:02:03'", type(w::Tag::kTime)), "-01:02:03");
  EXPECT_THROW(round_trip("'839:00:00'", type(w::Tag::kTime)), ConversionError);
  EXPECT_THROW(round_trip("Infinity", type(w::Tag::kF64)), ConversionError);
  EXPECT_EQ(round_trip("'1.5e3'", type(w::Tag::kF64)), "1500");
  EXPECT_EQ(round_trip("1e21", type(w::Tag::kDecimal, "DECIMAL")), "1000000000000000000000");
}

TEST_F(V8Values, Dates) {
  // A DATETIME is local time in the session's zone: midnight in Kyiv is 22:00 UTC in winter.
  std::array<std::byte, 64> buf{};
  Writer wr(buf);
  mle::v8w::Temporal t{w::Tag::kDatetime, 2026, 1, 15, 0, 0, 0, 0, false};
  mle::v8w::write_temporal(wr, t);
  Reader r(std::span<const std::byte>(buf.data(), wr.size()), 0);
  ParamType dt = type(w::Tag::kDatetime, "DATETIME");
  v8::Local<v8::Value> d = m_values->argument(r, dt);
  ASSERT_TRUE(d->IsDate());
  EXPECT_EQ(str(js("(d) => d.toISOString()").As<v8::Function>()->Call(m_ctx, v8::Undefined(m_iso), 1, &d)
                    .ToLocalChecked()),
            "2026-01-14T22:00:00.000Z");
  // a zero date is an error as an argument, an invalid Date in a row
  std::array<std::byte, 64> zero{};
  Writer zw(zero);
  mle::v8w::write_temporal(zw, mle::v8w::Temporal{w::Tag::kDate, 0, 0, 0, 0, 0, 0, 0, false});
  Reader zr(std::span<const std::byte>(zero.data(), zw.size()), 0);
  EXPECT_THROW(m_values->argument(zr, type(w::Tag::kDate, "DATE")), ConversionError);
  Reader zr2(std::span<const std::byte>(zero.data(), zw.size()), 0);
  EXPECT_TRUE(std::isnan(m_values->column(zr2, type(w::Tag::kDate, "DATE")).As<v8::Date>()->ValueOf()));
  // results back: the session's local date-time (00:00 on 2 July in Kyiv), the same instant again
  std::array<std::byte, 64> back{};
  Writer bw(back);
  m_values->write_as(bw, js("new Date(Date.UTC(2026, 6, 1, 21, 0, 0))"), dt);
  Reader br(std::span<const std::byte>(back.data(), bw.size()), 0);
  const mle::v8w::Temporal stored = mle::v8w::read_temporal((br.get<uint8_t>(), br), w::Tag::kDatetime);
  EXPECT_EQ(stored.day, 2);
  EXPECT_EQ(stored.hour, 0u);
  Reader br2(std::span<const std::byte>(back.data(), bw.size()), 0);
  v8::Local<v8::Value> again = m_values->column(br2, dt);
  EXPECT_EQ(again.As<v8::Date>()->ValueOf(), 1782939600000.0);
}

TEST_F(V8Values, Timestamps) {
  // A TIMESTAMP is an instant in microseconds, whatever the session's zone (§7.2).
  const ParamType ts = type(w::Tag::kTimestamp, "TIMESTAMP");
  const std::vector<std::byte> jan = bytes(0x0B, 0x00, 0x20, 0x21, 0x10, 0xD7, 0x0D, 0x06, 0x00);  // the spec's vector
  Reader r(jan);
  EXPECT_EQ(m_values->argument(r, ts).As<v8::Date>()->ValueOf(), 1704067200000.0);
  EXPECT_EQ(written("new Date(Date.UTC(2024, 0, 1))", ts), jan);
  EXPECT_EQ(round_trip("new Date(0)", ts), str(js("new Date(0)")));
  EXPECT_THROW(written("new Date(-1)", ts), ConversionError);
  EXPECT_THROW(written("new Date(Date.UTC(10000, 0, 1))", ts), ConversionError);
  EXPECT_THROW(written("new Date(NaN)", ts), ConversionError);
  EXPECT_THROW(written("'2024-01-01'", ts), ConversionError);
  // the zero timestamp: an error as an argument, an invalid Date in a row
  const std::vector<std::byte> zero = bytes(0x0B, 0, 0, 0, 0, 0, 0, 0, 0x80);
  Reader z(zero);
  EXPECT_THROW(m_values->argument(z, ts), ConversionError);
  EXPECT_EQ(z.left(), 0u);  // read completely before the error
  Reader z2(zero);
  EXPECT_TRUE(std::isnan(m_values->column(z2, ts).As<v8::Date>()->ValueOf()));
  // outside the wire's range
  const std::vector<std::byte> negative = bytes(0x0B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
  Reader n(negative);
  EXPECT_THROW(m_values->argument(n, ts), ProtocolFailure);
}

TEST_F(V8Values, Text) {
  // Ill-formed UTF-8 of the server's data is repaired: one U+FFFD per maximal subpart (§7.1).
  const std::vector<std::byte> ill = text_value(w::Tag::kString, "a\xF0\x80\x80z\xC3");
  Reader r(ill);
  const v8::Local<v8::Value> s = m_values->argument(r, type(w::Tag::kString, "TEXT"));
  EXPECT_EQ(mle::v8w::utf8(m_iso, s), "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDz\xEF\xBF\xBD");
  // Guest text: an unpaired surrogate becomes U+FFFD in data, and is refused in code and names.
  EXPECT_EQ(mle::v8w::utf8(m_iso, js("'a\\uD800b\\uDFFF'")), "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
  EXPECT_EQ(mle::v8w::utf8(m_iso, js("'\\uD83D\\uDE00'")), "\xF0\x9F\x98\x80");
  EXPECT_FALSE(mle::v8w::utf8_exact(m_iso, js("'SELECT \\uD800'").As<v8::String>()).has_value());
  EXPECT_FALSE(mle::v8w::utf8_exact(m_iso, js("'\\uDE00\\uD83D'").As<v8::String>()).has_value());
  EXPECT_EQ(mle::v8w::utf8_exact(m_iso, js("'gr\\u00FC\\u00DF \\uD83D\\uDE00'").As<v8::String>()).value(),
            "gr\xC3\xBC\xC3\x9F \xF0\x9F\x98\x80");
}

TEST_F(V8Values, Domains) {
  using mle::v8w::Temporal;
  using mle::v8w::valid_decimal;
  using mle::v8w::valid_temporal;
  for (const char *good : {"0", "-0", "007", "-12.50", "1.0"}) EXPECT_TRUE(valid_decimal(good)) << good;
  for (const char *bad : {"", "-", "+1", "1.", ".5", "1e3", "1.2.3", " 1", "1 "}) EXPECT_FALSE(valid_decimal(bad)) << bad;
  EXPECT_TRUE(valid_decimal(std::string(100, '9')));
  EXPECT_FALSE(valid_decimal(std::string(101, '9')));
  EXPECT_TRUE(valid_temporal(Temporal{w::Tag::kDate, 0, 0, 0, 0, 0, 0, 0, false}));
  EXPECT_TRUE(valid_temporal(Temporal{w::Tag::kDate, 9999, 12, 31, 0, 0, 0, 0, false}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kDate, 2024, 13, 1, 0, 0, 0, 0, false}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kDate, 2024, 1, 1, 1, 0, 0, 0, false}));
  EXPECT_TRUE(valid_temporal(Temporal{w::Tag::kTime, 0, 0, 0, 838, 59, 59, 0, true}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kTime, 0, 0, 0, 838, 59, 59, 1, false}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kTime, 0, 0, 1, 1, 0, 0, 0, false}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kDatetime, 2024, 1, 1, 24, 0, 0, 0, false}));
  EXPECT_FALSE(valid_temporal(Temporal{w::Tag::kDatetime, 2024, 1, 1, 0, 0, 0, 0, true}));
  // The spec's vectors: DATE 2024-02-29, TIME -01:02:03.5, DECIMAL "-12.50".
  EXPECT_EQ(written("'-01:02:03.5'", type(w::Tag::kTime, "TIME")),
            bytes(0x09, 0, 0, 0, 0, 1, 0, 0, 0, 2, 3, 0x20, 0xA1, 0x07, 0x00, 0x01));
  EXPECT_EQ(written("'-12.50'", type(w::Tag::kDecimal, "DECIMAL")),
            bytes(0x05, 6, 0, 0, 0, 0x2D, 0x31, 0x32, 0x2E, 0x35, 0x30));
  m_opt.set_zone("+00:00");
  EXPECT_EQ(written("new Date(Date.UTC(2024, 1, 29))", type(w::Tag::kDate, "DATE")),
            bytes(0x08, 0xE8, 0x07, 0x02, 0x1D, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
  // A VECTOR has 1 to 16383 dimensions; its elements are padded to 16 bytes from the body's start.
  EXPECT_THROW(written("[]", type(w::Tag::kVector)), ConversionError);
  EXPECT_THROW(written("new Float32Array(16384)", type(w::Tag::kVector)), ConversionError);
  const std::vector<std::byte> v = written("new Float32Array([1, 2])", type(w::Tag::kVector));
  EXPECT_EQ(v.size(), 24u);
  EXPECT_EQ(v[16], std::byte{0});
  EXPECT_EQ(v[19], std::byte{0x3F});
  // -0.0 is kept; any bit pattern is accepted from the host.
  EXPECT_EQ(written("-0", type(w::Tag::kF64)), bytes(0x03, 0, 0, 0, 0, 0, 0, 0, 0x80));
  const std::vector<std::byte> nan = bytes(0x03, 1, 0, 0, 0, 0, 0, 0xF0, 0x7F);
  Reader r(nan);
  EXPECT_TRUE(std::isnan(m_values->argument(r, type(w::Tag::kF64)).As<v8::Number>()->Value()));
}

TEST_F(V8Values, Malformed) {
  const auto fails = [this](const std::vector<std::byte> &message, w::Tag tag = w::Tag::kString) {
    Reader r(message);
    try {
      m_values->argument(r, type(tag));
      r.finish();
    } catch (const ProtocolFailure &) {
      return true;
    }
    return false;
  };
  EXPECT_TRUE(fails(bytes(6, 10, 0)));                    // a string of 10: cut
  EXPECT_TRUE(fails(bytes(99)));                          // an unknown tag
  EXPECT_TRUE(fails(bytes(0, 0)));                        // a trailing byte
  EXPECT_FALSE(fails(bytes(0)));
  EXPECT_TRUE(fails(text_value(w::Tag::kDecimal, "1e5"), w::Tag::kDecimal));
  EXPECT_TRUE(fails(bytes(13, 0, 0, 0, 0), w::Tag::kVector));                 // no dimensions
  // a VECTOR whose padding is not zero
  std::vector<std::byte> v = bytes(13, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x3F);
  EXPECT_FALSE(fails(v, w::Tag::kVector));
  v[7] = std::byte{1};
  EXPECT_TRUE(fails(v, w::Tag::kVector));
  // a DATE with a time, a bool that is 2
  EXPECT_TRUE(fails(bytes(8, 0xE8, 0x07, 2, 29, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), w::Tag::kDate));
  EXPECT_TRUE(fails(bytes(8, 0xE8, 0x07, 2, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2), w::Tag::kDate));
  // a count beyond the bytes left
  const std::vector<std::byte> list = bytes(5, 0, 0, 0, 1);
  Reader r(list);
  EXPECT_THROW(r.count(100, 1), ProtocolFailure);
  Reader r2(list);
  EXPECT_THROW(r2.count(4, 0x100), ProtocolFailure);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
