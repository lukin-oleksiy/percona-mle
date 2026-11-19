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

// The V8 worker's values (worker/v8/values.cc; exp design note 63): DECIMAL text within MySQL's
// limits, TIME text, time zones, arguments to JavaScript and results back by Oracle's rules (as
// the JVM worker's JsOracle), bounds of malformed messages. In an isolate of the distribution's V8.

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
    return str(m_values->column(r, t.type_name));
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
  m_opt.integer_type = w::IntegerType::kUnsafeBigint;
  std::array<std::byte, 16> buf{};
  Writer wr(buf);
  ASSERT_TRUE(wr.put(uint8_t(w::Tag::kI64)) && wr.put(int64_t{1} << 60));
  Reader r(std::span<const std::byte>(buf.data(), wr.size()), 0);
  EXPECT_TRUE(m_values->column(r, "BIGINT")->IsBigInt());
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
  EXPECT_TRUE(std::isnan(m_values->column(zr2, "DATE").As<v8::Date>()->ValueOf()));
  // results back: the session's local date-time (00:00 on 2 July in Kyiv), the same instant again
  std::array<std::byte, 64> back{};
  Writer bw(back);
  m_values->write_as(bw, js("new Date(Date.UTC(2026, 6, 1, 21, 0, 0))"), dt);
  Reader br(std::span<const std::byte>(back.data(), bw.size()), 0);
  const mle::v8w::Temporal stored = mle::v8w::read_temporal((br.get<uint8_t>(), br), w::Tag::kDatetime);
  EXPECT_EQ(stored.day, 2);
  EXPECT_EQ(stored.hour, 0u);
  Reader br2(std::span<const std::byte>(back.data(), bw.size()), 0);
  v8::Local<v8::Value> again = m_values->column(br2, "DATETIME");
  EXPECT_EQ(again.As<v8::Date>()->ValueOf(), 1782939600000.0);
}

TEST_F(V8Values, Malformed) {
  const std::array<std::byte, 3> short_msg{std::byte{6}, std::byte{10}, std::byte{0}};  // a string of 10: cut
  Reader r(short_msg, 0);
  EXPECT_THROW(m_values->argument(r, type(w::Tag::kString)), mle::v8w::Malformed);
  const std::array<std::byte, 1> bad{std::byte{99}};
  Reader r2(bad, 0);
  EXPECT_THROW(m_values->argument(r2, type(w::Tag::kString)), mle::v8w::Malformed);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
