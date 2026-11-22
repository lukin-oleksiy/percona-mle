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

// Unit tests of the codec of the MLE worker protocol (docs/protocol.md):
// every body layout of §9, the values and structures of §7 with their
// domains and caps, the chunk rules of §5.2, the header checks of §13, and
// the test vectors of §17 byte for byte.

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "mle/codec.h"
#include "mle/protocol.h"

namespace mle_codec_unittest {

using namespace mle;
using wire::MessageHeader;

// ---------------------------------------------------------------- helpers

std::string encode_value(const Value &v, size_t prefix = 0) {
  std::string out(prefix, 'x');
  Writer(&out).value(v);
  return out;
}

bool decode_value(const std::string &bytes, Value *v, size_t prefix = 0,
                  std::string *error = nullptr,
                  Sender from = Sender::kWorker) {
  Reader r(bytes, from);
  for (size_t i = 0; i < prefix; i++) {
    uint8_t skip;
    r.u8(&skip);
  }
  bool ok = r.value(v) && r.finish();
  if (error != nullptr) *error = r.error();
  return ok;
}

Value round_trip(const Value &v) {
  Value out;
  std::string error;
  EXPECT_TRUE(decode_value(encode_value(v), &out, 0, &error)) << error;
  return out;
}

/** Bytes from hexadecimal text; blanks are skipped. */
std::string hex(std::string_view text) {
  std::string out;
  int high = -1;
  for (const char c : text) {
    if (c == ' ' || c == '\n') continue;
    const int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
    if (high < 0) {
      high = d;
    } else {
      out += static_cast<char>(high << 4 | d);
      high = -1;
    }
  }
  return out;
}

Temporal date(uint16_t y, uint8_t m, uint8_t d) {
  Temporal t;
  t.year = y, t.month = m, t.day = d;
  return t;
}

Temporal datetime(uint16_t y, uint8_t mo, uint8_t d, uint32_t h, uint8_t mi,
                  uint8_t s, uint32_t us) {
  Temporal t = date(y, mo, d);
  t.hour = h, t.minute = mi, t.second = s, t.microsecond = us;
  return t;
}

Temporal time_of(bool negative, uint32_t h, uint8_t mi, uint8_t s,
                 uint32_t us) {
  Temporal t = datetime(0, 0, 0, h, mi, s, us);
  t.negative = negative;
  return t;
}

std::vector<Value> all_kinds_of_values() {
  std::vector<float> v768(768);
  for (size_t i = 0; i < v768.size(); i++) v768[i] = 0.5f * float(i) - 7.25f;
  return {
      Value::null(),
      Value::i64(0),
      Value::i64(std::numeric_limits<int64_t>::min()),
      Value::i64(std::numeric_limits<int64_t>::max()),
      Value::u64(std::numeric_limits<uint64_t>::max()),
      Value::f64(-0.0),
      Value::f64(std::numeric_limits<double>::denorm_min()),
      Value::f64(std::numeric_limits<double>::quiet_NaN()),
      Value::f64(-std::numeric_limits<double>::infinity()),
      Value::f32(3.25f),
      Value::f32(std::numeric_limits<float>::max()),
      Value::decimal("0"),
      Value::decimal("-0"),
      Value::decimal("007.50"),
      Value::decimal("-12345678901234567890123456789012345.123456789012345678"
                     "901234567890"),
      Value::decimal(std::string(100, '9')),
      Value::string(""),
      Value::string("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"),  // é € 😀
      Value::string(std::string("a\0b", 3)),
      Value::bytes(std::string("a\0b\xff\xfe", 5)),
      Value::bytes(std::string(70000, '\x80')),
      Value::temporal(Tag::kDate, date(2026, 11, 10)),
      Value::temporal(Tag::kDate, date(0, 0, 0)),
      Value::temporal(Tag::kDate, date(2026, 0, 31)),
      Value::temporal(Tag::kDatetime,
                      datetime(9999, 12, 31, 23, 59, 59, 999999)),
      Value::temporal(Tag::kTime, time_of(true, 838, 59, 59, 0)),
      // All of hour 838 is in TIME's range.
      Value::temporal(Tag::kTime, time_of(false, 838, 0, 0, 0)),
      Value::temporal(Tag::kTime, time_of(true, 838, 30, 0, 123456)),
      Value::temporal(Tag::kTime, time_of(false, 838, 59, 58, 999999)),
      Value::temporal(Tag::kTime, time_of(false, 0, 0, 0, 1)),
      Value::temporal(Tag::kTime, time_of(true, 0, 0, 0, 0)),
      Value::timestamp(0),
      Value::timestamp(1704067200000000),
      Value::timestamp(wire::kMaxTimestamp),
      Value::timestamp(wire::kZeroTimestamp),
      Value::json(R"({"a":[1,2,{"b":null}],"c":"é"})"),
      Value::json("not JSON at all"),  // its syntax is the server's to check
      Value::vector({1.0f}),
      Value::vector({1.0f, -2.0f, std::numeric_limits<float>::quiet_NaN()}),
      Value::vector(v768),
      Value::vector(std::vector<float>(wire::kMaxVectorDimensions, 1.5f)),
  };
}

// ---------------------------------------------------------------- layout

TEST(MleProtocol, RegionLayout) {
  EXPECT_EQ(wire::region_size(wire::kDefaultArenaSize),
            wire::kHeaderSize + (1u << 20));
  EXPECT_EQ(offsetof(wire::RegionHeader, magic), 0u);
  EXPECT_EQ(offsetof(wire::RegionHeader, version), 4u);
  EXPECT_EQ(offsetof(wire::RegionHeader, region_size), 8u);
  EXPECT_EQ(offsetof(wire::RegionHeader, arena_size), 16u);
  EXPECT_EQ(offsetof(wire::RegionHeader, to_w), 64u);
  EXPECT_EQ(offsetof(wire::RegionHeader, to_h), 128u);
  EXPECT_EQ(offsetof(wire::RegionHeader, w_sleep), 192u);
  EXPECT_EQ(offsetof(wire::RegionHeader, h_sleep), 256u);
  EXPECT_EQ(offsetof(wire::RegionHeader, msg), 320u);
  EXPECT_EQ(offsetof(wire::RegionHeader, stats), 384u);
  EXPECT_EQ(sizeof(wire::RegionHeader), 4096u);
  EXPECT_EQ(offsetof(MessageHeader, total_len), 24u);
  EXPECT_EQ(offsetof(MessageHeader, call_seq), 32u);
  EXPECT_EQ(offsetof(wire::WorkerStats, state), 24u);
  EXPECT_EQ(offsetof(wire::WorkerStats, reserved), 28u);
  EXPECT_EQ(uint32_t{wire::kMore}, 1u);
  EXPECT_EQ(uint32_t{wire::kTainted}, 2u);
  EXPECT_EQ(wire::kMagic, uint32_t('M') | uint32_t('L') << 8 |
                              uint32_t('E') << 16 | uint32_t('1') << 24);
  EXPECT_EQ(wire::kVersion, 0x00010000u);
  EXPECT_EQ(wire::version_major(0x00020007), 2u);
  EXPECT_EQ(wire::version_minor(0x00020007), 7u);
}

TEST(MleProtocol, ArenaSizes) {
  EXPECT_TRUE(wire::is_valid_arena_size(64u << 10));
  EXPECT_TRUE(wire::is_valid_arena_size(1u << 30));
  EXPECT_FALSE(wire::is_valid_arena_size((64u << 10) - 4096));
  EXPECT_FALSE(wire::is_valid_arena_size((64u << 10) + 16));
  EXPECT_FALSE(wire::is_valid_arena_size((1ull << 30) + 4096));
}

TEST(MleProtocol, Ops) {
  for (uint32_t op : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 14u})
    EXPECT_TRUE(wire::is_assigned_op(op)) << op;
  for (uint32_t op : {0u, 7u, 15u, 99u})
    EXPECT_FALSE(wire::is_assigned_op(op)) << op;
}

// ---------------------------------------------------------------- vectors

// The test vectors of the specification, §17, in both directions.
TEST(MleSpecVectors, CallBody) {
  const std::string bytes = hex(
      "07 00 00 00 00 00 00 00"
      "01 00 00 00 00 00 00 00"
      "02 00 00 00"
      "01 FE FF FF FF FF FF FF FF"
      "0D 02 00 00 00"
      "00 00 00 00 00 00 00 00 00 00 00 00 00 00"
      "00 00 80 3F 00 00 00 40");
  ASSERT_EQ(bytes.size(), 56u);
  CallMsg m;
  m.routine_id = 7;
  m.library_epoch = 1;
  m.args = {Value::i64(-2), Value::vector({1.0f, 2.0f})};
  std::string out;
  encode(m, &out);
  EXPECT_EQ(out, bytes);
  CallMsg back;
  std::string error;
  ASSERT_TRUE(decode(bytes, &back, &error)) << error;
  EXPECT_EQ(back, m);
}

TEST(MleSpecVectors, CallHeader) {
  const std::string bytes = hex(
      "03 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00"
      "38 00 00 00  00 00 00 00"
      "38 00 00 00 00 00 00 00"
      "03 00 00 00 00 00 00 00");
  MessageHeader h{};
  h.op = static_cast<uint32_t>(wire::Op::kCall);
  h.len = 56;
  h.total_len = 56;
  h.call_seq = 3;
  ASSERT_EQ(bytes.size(), sizeof h);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(&h), sizeof h), bytes);
}

TEST(MleSpecVectors, ResultBody) {
  const std::string bytes = hex(
      "01"
      "01 2A 00 00 00 00 00 00 00"
      "00 00 00 00"
      "00 00 00 00"
      "00 00 00 00"
      "00"
      "00 00 00 00"
      "00");
  ASSERT_EQ(bytes.size(), 28u);
  ResultMsg m;
  m.ret = Value::i64(42);
  std::string out;
  encode(m, &out);
  EXPECT_EQ(out, bytes);
  ResultMsg back;
  std::string error;
  ASSERT_TRUE(decode(bytes, &back, &error)) << error;
  EXPECT_EQ(back, m);
}

TEST(MleSpecVectors, ReadyBody) {
  const std::string bytes = hex(
      "00 00 01 00"
      "02 00 00 00 6F 6B"
      "00 00 00 00 00 00 00 00"
      "02 00 00 00 76 38"
      "01 00 00 00"
      "02 00 00 00 6A 73"
      "01 00 00 00 58");
  ASSERT_EQ(bytes.size(), 39u);
  ReadyMsg m;
  m.info = "ok";
  m.worker_class = "v8";
  m.languages = {"js"};
  m.runtime = "X";
  std::string out;
  encode(m, &out);
  EXPECT_EQ(out, bytes);
  ReadyMsg back;
  std::string error;
  ASSERT_TRUE(decode(bytes, &back, &error)) << error;
  EXPECT_EQ(back, m);
  EXPECT_EQ(back.version, 0x00010000u);
}

TEST(MleSpecVectors, SqlBody) {
  const std::string bytes = hex(
      "00"
      "01 00 00 00 00 00 00 00"
      "08 00 00 00 53 45 4C 45 43 54 20 31"
      "00 00 00 00"
      "00"
      "00 00 00 00"
      "00 00 00 00");
  ASSERT_EQ(bytes.size(), 34u);
  SqlMsg m;
  m.action = wire::SqlAction::kExecute;
  m.stmt_id = 1;
  m.statement = "SELECT 1";
  std::string out;
  encode(m, &out);
  EXPECT_EQ(out, bytes);
  SqlMsg back;
  std::string error;
  ASSERT_TRUE(decode(bytes, &back, &error)) << error;
  EXPECT_EQ(back, m);
}

TEST(MleSpecVectors, SqlResultError) {
  const std::string bytes = hex(
      "00"
      "26 04 00 00"
      "05 00 00 00 32 33 30 30 30"
      "03 00 00 00 64 75 70"
      "FF FF FF FF"
      "01 00 00 00 00 00 00 00");
  ASSERT_EQ(bytes.size(), 33u);
  SqlResultMsg m;
  m.kind = SqlResultKind::kError;
  m.sql_errno = 1062;
  m.sqlstate = "23000";
  m.message = "dup";
  m.error_id = 1;
  std::string out;
  encode(m, &out);
  EXPECT_EQ(out, bytes);
  SqlResultMsg back;
  std::string error;
  ASSERT_TRUE(decode(bytes, &back, &error)) << error;
  EXPECT_EQ(back.kind, SqlResultKind::kError);
  EXPECT_EQ(back.sql_errno, 1062u);
  EXPECT_EQ(back.sqlstate, "23000");
  EXPECT_EQ(back.message, "dup");
  EXPECT_EQ(back.failed_row, wire::kNoRow);
  EXPECT_EQ(back.error_id, 1u);
}

TEST(MleSpecVectors, Values) {
  struct Case {
    const char *what;
    Value value;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {"DATE 2024-02-29", Value::temporal(Tag::kDate, date(2024, 2, 29)),
       hex("08 E8 07 02 1D 00 00 00 00 00 00 00 00 00 00 00")},
      {"TIME -01:02:03.5",
       Value::temporal(Tag::kTime, time_of(true, 1, 2, 3, 500000)),
       hex("09 00 00 00 00 01 00 00 00 02 03 20 A1 07 00 01")},
      {"DECIMAL -12.50", Value::decimal("-12.50"),
       hex("05 06 00 00 00 2D 31 32 2E 35 30")},
      {"the zero TIMESTAMP", Value::timestamp(wire::kZeroTimestamp),
       hex("0B 00 00 00 00 00 00 00 80")},
      {"TIMESTAMP 2024-01-01 00:00:00 UTC", Value::timestamp(1704067200000000),
       hex("0B 00 20 21 10 D7 0D 06 00")},
  };
  for (const Case &c : cases) {
    EXPECT_EQ(encode_value(c.value), c.bytes) << c.what;
    Value back;
    std::string error;
    EXPECT_TRUE(decode_value(c.bytes, &back, 0, &error)) << c.what << error;
    EXPECT_EQ(back, c.value) << c.what;
  }
}

// A message of arena_size + 10 bytes with a 65536-byte arena.
TEST(MleSpecVectors, Chunks) {
  const uint32_t arena = 65536;
  const std::string body(arena + 10, 'b');
  MessageHeader first{};
  first.op = static_cast<uint32_t>(wire::Op::kResult);
  first.depth = 2;
  first.call_seq = 9;
  first.len = 65536;
  first.flags = 1;
  first.total_len = 65546;
  MessageHeader second = first;
  second.len = 10;
  second.flags = 0;
  std::string error;
  EXPECT_TRUE(check_worker_header(first, arena, &error)) << error;
  EXPECT_TRUE(check_worker_header(second, arena, &error)) << error;
  Assembler a(arena, wire::kMaxWorkerMessage);
  EXPECT_EQ(a.add(first, body.data()), Assembler::Step::kNeedMore);
  EXPECT_EQ(a.add(second, body.data() + arena), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), body);
  // The CONTINUE between them.
  MessageHeader go{};
  go.op = 10;
  go.depth = 2;
  go.call_seq = 9;
  EXPECT_TRUE(check_worker_header(go, arena, &error)) << error;
}

TEST(MleSpecVectors, Interrupt) {
  // The datagram itself is checked where the host sends it
  // (worker_protocol-t); here its constants.
  EXPECT_EQ(static_cast<uint8_t>(wire::Control::kInterrupt), 1);
  EXPECT_EQ(wire::kInterruptSize, 9u);
}

// ---------------------------------------------------------------- values

TEST(MleCodec, ValueRoundTrip) {
  for (const Value &v : all_kinds_of_values()) {
    SCOPED_TRACE(tag_name(v.tag()));
    EXPECT_EQ(round_trip(v), v);
  }
}

TEST(MleCodec, ValueEqualityIsExact) {
  EXPECT_NE(Value::f64(0.0), Value::f64(-0.0));
  EXPECT_EQ(Value::f64(std::nan("")), Value::f64(std::nan("")));
  EXPECT_NE(Value::i64(1), Value::u64(1));
  EXPECT_NE(Value::string("x"), Value::bytes("x"));
  EXPECT_NE(Value::i64(5), Value::timestamp(5));
  EXPECT_NE(Value::temporal(Tag::kDatetime, date(2026, 1, 1)),
            Value::temporal(Tag::kDate, date(2026, 1, 1)));
}

TEST(MleCodec, NegativeZeroIsPreserved) {
  EXPECT_TRUE(std::signbit(round_trip(Value::f64(-0.0)).as_f64()));
  EXPECT_TRUE(std::signbit(round_trip(Value::f32(-0.0f)).as_f32()));
}

TEST(MleCodec, VectorDataIsAligned) {
  const Value v = Value::vector({1.0f, 2.0f, 3.0f});
  for (size_t prefix = 0; prefix < 40; prefix++) {
    std::string bytes = encode_value(v, prefix);
    // tag + u32 dimensions, then padding to 16 counted from the start.
    size_t data = prefix + 1 + 4;
    data += (16 - data % 16) % 16;
    ASSERT_EQ(bytes.size(), data + 3 * sizeof(float)) << prefix;
    float first;
    std::memcpy(&first, bytes.data() + data, sizeof first);
    EXPECT_EQ(first, 1.0f);
    Value back;
    EXPECT_TRUE(decode_value(bytes, &back, prefix));
    EXPECT_EQ(back, v);
  }
}

TEST(MleCodec, EncodingIsLittleEndian) {
  EXPECT_EQ(encode_value(Value::i64(0x0102030405060708)),
            std::string("\x01\x08\x07\x06\x05\x04\x03\x02\x01", 9));
  EXPECT_EQ(encode_value(Value::string("ab")),
            std::string("\x06\x02\x00\x00\x00"
                        "ab",
                        7));
}

// Every value in all_kinds_of_values() fails to decode when cut short.
TEST(MleCodec, TruncatedValuesAreRejected) {
  for (const Value &v : all_kinds_of_values()) {
    std::string bytes = encode_value(v);
    if (bytes.size() > 2000) bytes.resize(2000);  // keep the test fast
    for (size_t n = 0; n < bytes.size(); n++) {
      Value out;
      EXPECT_FALSE(decode_value(bytes.substr(0, n), &out))
          << tag_name(v.tag()) << " cut at " << n;
    }
  }
}

struct BadValue {
  const char *what;
  std::string bytes;
};

std::string tagged_str(Tag tag, std::string_view s) {
  std::string out;
  Writer w(&out);
  w.u8(uint8_t(tag));
  w.str(s);
  return out;
}

std::string temporal_bytes(Tag tag, const Temporal &t, uint8_t negative) {
  std::string out;
  Writer w(&out);
  w.u8(uint8_t(tag));
  w.u16(t.year);
  w.u8(t.month);
  w.u8(t.day);
  w.u32(t.hour);
  w.u8(t.minute);
  w.u8(t.second);
  w.u32(t.microsecond);
  w.u8(negative);
  return out;
}

std::string timestamp_bytes(int64_t us) {
  std::string out;
  Writer w(&out);
  w.u8(uint8_t(Tag::kTimestamp));
  w.i64(us);
  return out;
}

std::string vector_head(uint32_t dims) {
  std::string out;
  Writer w(&out);
  w.u8(uint8_t(Tag::kVector));
  w.u32(dims);
  return out;
}

// The domains of §7.2: what breaks them is malformed.
TEST(MleCodec, BadValuesAreRejected) {
  std::string dirty_padding = encode_value(Value::vector({1.0f}));
  dirty_padding[6] = 1;  // padding starts after tag + dimensions
  std::string long_string;
  {
    Writer w(&long_string);
    w.u8(uint8_t(Tag::kBytes));
    w.u32(0xFFFFFFFF);
  }
  const std::vector<BadValue> cases = {
      {"unknown tag", std::string(1, char(wire::kMaxTag + 1))},
      {"tag 255", std::string(1, '\xff')},
      {"length beyond message", long_string},
      {"lone continuation", tagged_str(Tag::kString, "\x80")},
      {"overlong slash", tagged_str(Tag::kString, "\xc0\xaf")},
      {"overlong 3 bytes", tagged_str(Tag::kString, "\xe0\x80\xaf")},
      {"surrogate", tagged_str(Tag::kString, "\xed\xa0\x80")},
      {"beyond U+10FFFF", tagged_str(Tag::kString, "\xf4\x90\x80\x80")},
      {"truncated sequence", tagged_str(Tag::kJson, "\xe2\x82")},
      {"bad lead byte", tagged_str(Tag::kString, "\xff")},
      {"decimal empty", tagged_str(Tag::kDecimal, "")},
      {"decimal sign only", tagged_str(Tag::kDecimal, "-")},
      {"decimal plus", tagged_str(Tag::kDecimal, "+1")},
      {"decimal trailing dot", tagged_str(Tag::kDecimal, "1.")},
      {"decimal leading dot", tagged_str(Tag::kDecimal, ".5")},
      {"decimal exponent", tagged_str(Tag::kDecimal, "1e5")},
      {"decimal space", tagged_str(Tag::kDecimal, "1 ")},
      {"decimal too long", tagged_str(Tag::kDecimal, std::string(101, '9'))},
      {"date with time",
       temporal_bytes(Tag::kDate, datetime(2026, 1, 1, 1, 0, 0, 0), 0)},
      {"date month 13", temporal_bytes(Tag::kDate, date(2026, 13, 1), 0)},
      {"date day 32", temporal_bytes(Tag::kDate, date(2026, 1, 32), 0)},
      {"date year 10000", temporal_bytes(Tag::kDate, date(10000, 1, 1), 0)},
      {"negative date", temporal_bytes(Tag::kDate, date(2026, 1, 1), 1)},
      {"datetime hour 24",
       temporal_bytes(Tag::kDatetime, datetime(2026, 1, 1, 24, 0, 0, 0), 0)},
      {"datetime minute 60",
       temporal_bytes(Tag::kDatetime, datetime(2026, 1, 1, 0, 60, 0, 0), 0)},
      {"datetime micro 1e6",
       temporal_bytes(Tag::kDatetime, datetime(2026, 1, 1, 0, 0, 0, 1000000),
                      0)},
      {"negative datetime",
       temporal_bytes(Tag::kDatetime, datetime(2026, 1, 1, 0, 0, 0, 0), 1)},
      {"time hour 839",
       temporal_bytes(Tag::kTime, time_of(false, 839, 0, 0, 0), 0)},
      {"time beyond max",
       temporal_bytes(Tag::kTime, time_of(false, 838, 59, 59, 1), 0)},
      {"time with date",
       temporal_bytes(Tag::kTime, datetime(0, 0, 1, 1, 0, 0, 0), 0)},
      {"negative flag 2",
       temporal_bytes(Tag::kTime, time_of(false, 1, 0, 0, 0), 2)},
      {"timestamp before the epoch", timestamp_bytes(-1)},
      {"timestamp after year 9999", timestamp_bytes(wire::kMaxTimestamp + 1)},
      {"timestamp next to the zero value",
       timestamp_bytes(wire::kZeroTimestamp + 1)},
      {"a civil timestamp",
       temporal_bytes(Tag::kTimestamp, datetime(2026, 1, 1, 0, 0, 0, 0), 0)},
      {"vector without dimensions", vector_head(0)},
      {"vector too large", vector_head(wire::kMaxVectorDimensions + 1)},
      {"vector padding not zero", dirty_padding},
  };
  for (const BadValue &c : cases) {
    Value v;
    std::string error;
    EXPECT_FALSE(decode_value(c.bytes, &v, 0, &error)) << c.what;
    EXPECT_FALSE(error.empty()) << c.what;
  }
}

// STRING and JSON are lstr: from a worker well-formed, from the host any
// bytes of the server (§7.1). DECIMAL and str stay strict.
TEST(MleCodec, TextValuesFromTheHost) {
  for (const Tag tag : {Tag::kString, Tag::kJson}) {
    const std::string bytes = tagged_str(tag, "caf\xe9 \xed\xa0\x80");
    Value v;
    EXPECT_FALSE(decode_value(bytes, &v, 0, nullptr, Sender::kWorker));
    ASSERT_TRUE(decode_value(bytes, &v, 0, nullptr, Sender::kHost));
    EXPECT_EQ(v.as_text(), "caf\xe9 \xed\xa0\x80");
  }
  Value v;
  EXPECT_FALSE(decode_value(tagged_str(Tag::kDecimal, "1\xe9"), &v, 0, nullptr,
                            Sender::kHost));
  // In a message: a CALL's argument and a row value.
  CallMsg call;
  call.args = {Value::string("\xff")};
  std::string bytes;
  encode(call, &bytes);
  CallMsg back;
  EXPECT_TRUE(decode(bytes, &back, nullptr));
  // The same value in a worker's RESULT is malformed.
  ResultMsg result;
  result.ret = Value::string("\xff");
  encode(result, &bytes);
  ResultMsg result_back;
  EXPECT_FALSE(decode(bytes, &result_back, nullptr));
}

// A float that is not finite decodes (it is not malformed) and is found by
// all_finite(): the host turns it into an error (§7.2, §13).
TEST(MleCodec, NonFiniteFloatsAreNotMalformed) {
  const double inf = std::numeric_limits<double>::infinity();
  const float nanf = std::numeric_limits<float>::quiet_NaN();
  for (const Value &v :
       {Value::f64(inf), Value::f64(-inf), Value::f64(std::nan("")),
        Value::f32(nanf), Value::f32(-std::numeric_limits<float>::infinity()),
        Value::vector({1.0f, nanf}),
        Value::vector({std::numeric_limits<float>::infinity()})}) {
    EXPECT_EQ(round_trip(v), v);
    EXPECT_FALSE(is_finite(v)) << tag_name(v.tag());
  }
  for (const Value &v :
       {Value::f64(-0.0), Value::f64(std::numeric_limits<double>::max()),
        Value::f32(std::numeric_limits<float>::denorm_min()),
        Value::vector({1.0f, -2.0f}), Value::null(), Value::string("NaN"),
        Value::i64(1)})
    EXPECT_TRUE(is_finite(v)) << tag_name(v.tag());
  const std::vector<Value> good = {Value::i64(1), Value::f64(2.5)};
  const std::vector<Value> bad = {Value::i64(1), Value::vector({nanf})};
  EXPECT_TRUE(all_finite(good));
  EXPECT_FALSE(all_finite(bad));
  SqlMsg sql;
  sql.statement = "SELECT ?";
  sql.params = bad;
  std::string bytes, error;
  encode(sql, &bytes);
  SqlMsg back;
  EXPECT_TRUE(decode(bytes, &back, &error)) << error;
}

TEST(MleCodec, TrailingBytesAreRejected) {
  std::string bytes = encode_value(Value::i64(1)) + "x";
  Value v;
  std::string error;
  EXPECT_FALSE(decode_value(bytes, &v, 0, &error));
  EXPECT_NE(error.find("trailing"), std::string::npos) << error;
}

TEST(MleCodec, Utf8Validation) {
  EXPECT_TRUE(is_valid_utf8(""));
  EXPECT_TRUE(is_valid_utf8("\x7f"));
  EXPECT_TRUE(is_valid_utf8("\xc2\x80"));
  EXPECT_TRUE(is_valid_utf8("\xef\xbf\xbf"));
  EXPECT_TRUE(is_valid_utf8("\xf4\x8f\xbf\xbf"));  // U+10FFFF
  EXPECT_TRUE(is_valid_utf8(std::string("a\0b", 3)));
  EXPECT_FALSE(is_valid_utf8("\xc1\xbf"));          // overlong
  EXPECT_FALSE(is_valid_utf8("\xf0\x8f\xbf\xbf"));  // overlong 4 bytes
  EXPECT_FALSE(is_valid_utf8("\xed\xbf\xbf"));      // surrogate
  EXPECT_FALSE(is_valid_utf8("\xf8\x88\x80\x80\x80"));
}

TEST(MleCodec, SmallDomains) {
  EXPECT_TRUE(is_valid_identifier("js"));
  EXPECT_TRUE(is_valid_identifier("a_1"));
  EXPECT_TRUE(is_valid_identifier(std::string(32, 'a')));
  EXPECT_FALSE(is_valid_identifier(""));
  EXPECT_FALSE(is_valid_identifier(std::string(33, 'a')));
  EXPECT_FALSE(is_valid_identifier("JS"));
  EXPECT_FALSE(is_valid_identifier("a-b"));
  EXPECT_FALSE(is_valid_identifier("a.b"));
  EXPECT_TRUE(is_valid_sqlstate(""));
  EXPECT_TRUE(is_valid_sqlstate("42S02"));
  EXPECT_FALSE(is_valid_sqlstate("42s02"));
  EXPECT_FALSE(is_valid_sqlstate("4200"));
  EXPECT_FALSE(is_valid_sqlstate("420000"));
  EXPECT_TRUE(is_valid_library_name("x"));
  EXPECT_TRUE(is_valid_library_name(std::string(256, 'x')));
  EXPECT_FALSE(is_valid_library_name(""));
  EXPECT_FALSE(is_valid_library_name(std::string(257, 'x')));
  EXPECT_FALSE(is_valid_library_name(std::string("a\0b", 3)));
  EXPECT_TRUE(is_valid_timestamp(0));
  EXPECT_TRUE(is_valid_timestamp(wire::kMaxTimestamp));
  EXPECT_TRUE(is_valid_timestamp(wire::kZeroTimestamp));
  EXPECT_FALSE(is_valid_timestamp(-1));
}

// ---------------------------------------------------------------- messages

template <class M>
M message_round_trip(const M &m) {
  std::string bytes;
  encode(m, &bytes);
  M back;
  std::string error;
  EXPECT_TRUE(decode(bytes, &back, &error)) << error;
  // Every proper prefix is rejected, never over-read.
  for (size_t n = 0; n < bytes.size() && n < 3000; n++) {
    M cut;
    EXPECT_FALSE(decode(std::string_view(bytes).substr(0, n), &cut, nullptr))
        << "accepted a prefix of " << n << " of " << bytes.size();
  }
  M extra;
  EXPECT_FALSE(decode(bytes + '\0', &extra, nullptr));
  return back;
}

template <class M>
bool decodes(const M &m, std::string *error = nullptr) {
  std::string bytes;
  encode(m, &bytes);
  M back;
  return decode(bytes, &back, error);
}

ParamType vector_type(uint32_t dim) {
  ParamType t;
  t.tag = Tag::kVector;
  t.length = dim;
  t.type_name = "VECTOR";
  return t;
}

ReadyMsg good_ready() {
  ReadyMsg m;
  m.info = "ready in 92 ms";
  m.worker_class = "jvm";
  m.languages = {"java", "js", "python"};
  m.runtime = "GraalVM CE 25.5.5+3";
  return m;
}

TEST(MleCodec, ReadyRoundTrip) {
  ReadyMsg m = good_ready();
  EXPECT_EQ(message_round_trip(m), m);
  m.version = 0x00010007;
  m.features = 0x8000000000000001;
  EXPECT_EQ(message_round_trip(m), m);
}

TEST(MleCodec, ReadyDomains) {
  auto bad = [](auto change) {
    ReadyMsg m = good_ready();
    change(m);
    return !decodes(m);
  };
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.worker_class = ""; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.worker_class = "JVM"; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.worker_class = std::string(33, 'a'); }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.languages.clear(); }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.languages = {"js", "js"}; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.languages = {"js", ""}; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.languages = {"c++"}; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.languages = {"\xff"}; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) {
    m.languages.clear();
    for (int i = 0; i < 17; i++) m.languages.push_back("l" + std::to_string(i));
  }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.info = "\xff"; }));
  EXPECT_TRUE(bad([](ReadyMsg &m) { m.runtime = "\xc0\xaf"; }));
  EXPECT_FALSE(bad([](ReadyMsg &m) {
    m.languages.clear();
    for (int i = 0; i < 16; i++) m.languages.push_back("l" + std::to_string(i));
  }));
}

// A failed READY may end after info; of any READY the version, or version
// and info, can be read without the rest (§9.1, §14).
TEST(MleCodec, ReadyHead) {
  std::string bytes;
  encode_failed_ready(0x00020000, "worker speaks 2.0, host 1.0", &bytes);
  EXPECT_EQ(bytes.size(), 4u + 4u + 27u);
  uint32_t version = 0;
  std::string info, error;
  ASSERT_TRUE(decode_ready_head(bytes, &version, &info, &error)) << error;
  EXPECT_EQ(version, 0x00020000u);
  EXPECT_EQ(info, "worker speaks 2.0, host 1.0");
  // What follows is ignored, whatever it is.
  ASSERT_TRUE(decode_ready_head(bytes + "\xff\xff\xff", &version, &info, &error));
  ASSERT_TRUE(decode_ready_version(bytes.substr(0, 4) + "\xff", &version, &error));
  EXPECT_EQ(version, 0x00020000u);
  EXPECT_FALSE(decode_ready_version(bytes.substr(0, 3), &version, &error));
  EXPECT_FALSE(decode_ready_head(bytes.substr(0, 10), &version, &info, &error));
  encode_failed_ready(wire::kVersion, "\xff", &bytes);
  EXPECT_FALSE(decode_ready_head(bytes, &version, &info, &error));
  // The short body is not a whole READY.
  ReadyMsg whole;
  encode_failed_ready(wire::kVersion, "no heap", &bytes);
  EXPECT_FALSE(decode(bytes, &whole, &error));
  // A whole READY is a valid failed one.
  encode(good_ready(), &bytes);
  ASSERT_TRUE(decode_ready_head(bytes, &version, &info, &error));
  EXPECT_EQ(info, "ready in 92 ms");
}

TEST(MleCodec, PrepareRoundTrip) {
  PrepareMsg m;
  m.routine_id = 0xfeedfacecafebeef;
  m.language = "python";
  m.name = "nearest_rows";
  m.entry = "nearest";
  m.source = "import numpy as np\ndef nearest(q, k):\n  return 1\n";
  ParamType dec;
  dec.tag = Tag::kDecimal;
  dec.precision = 65;
  dec.scale = 30;
  dec.type_name = "DECIMAL";
  ParamType u;
  u.tag = Tag::kU64;
  u.is_unsigned = true;
  u.type_name = "BIGINT";
  m.params = {{"q", ParamMode::kIn, vector_type(768)},
              {"k", ParamMode::kInOut, u},
              {"total", ParamMode::kOut, dec}};
  m.returns = vector_type(3);
  m.imports = {{"jslib", "lib1", "mylib"}, {"db1", "theirlib", ""}};
  m.schema = "db1";
  m.check = true;
  m.library_epoch = 0x0102030405060708;
  EXPECT_EQ(message_round_trip(m), m);
  // library_epoch is the last field.
  std::string bytes;
  encode(m, &bytes);
  EXPECT_EQ(bytes.substr(bytes.size() - 9),
            std::string("\x01\x08\x07\x06\x05\x04\x03\x02\x01", 9));
  m.returns.reset();
  m.params.clear();
  m.imports.clear();
  EXPECT_EQ(message_round_trip(m), m);
}

TEST(MleCodec, LibraryMessagesRoundTrip) {
  ParseLibraryMsg p;
  p.language = "js";
  p.name = "lib1";
  p.body = "export function f(n) { return n }";
  EXPECT_EQ(message_round_trip(p), p);
  LibraryMsg l;
  l.schema = "db1";
  l.name = "theirlib";
  l.known_version = 0x9a91d68a5f842a47;
  EXPECT_EQ(message_round_trip(l), l);
  LibraryResultMsg r;
  r.status = wire::LibraryStatus::kBody;
  r.language = "js";
  r.body = "import {foo} from \"/db1/mylib\"";
  r.version = 17;
  EXPECT_EQ(message_round_trip(r), r);
  r = LibraryResultMsg();
  r.status = wire::LibraryStatus::kUnchanged;
  r.language = "js";
  r.version = 17;
  EXPECT_EQ(message_round_trip(r), r);
  r = LibraryResultMsg();
  r.message = "library other.secret is not importable";
  EXPECT_EQ(message_round_trip(r), r);
  std::string bytes;
  encode(r, &bytes);
  bytes[0] = 3;  // no such status
  LibraryResultMsg bad;
  EXPECT_FALSE(decode(bytes, &bad, nullptr));
}

// LIBRARY's schema and name: 1 to 256 bytes without U+0000 (§9.10).
TEST(MleCodec, LibraryNames) {
  auto bad = [](std::string schema, std::string name) {
    LibraryMsg m;
    m.schema = std::move(schema);
    m.name = std::move(name);
    return !decodes(m);
  };
  EXPECT_FALSE(bad("s", std::string(256, 'n')));
  EXPECT_TRUE(bad("", "n"));
  EXPECT_TRUE(bad("s", ""));
  EXPECT_TRUE(bad(std::string(257, 's'), "n"));
  EXPECT_TRUE(bad("s", std::string(257, 'n')));
  EXPECT_TRUE(bad("s", std::string("a\0b", 3)));
  EXPECT_TRUE(bad("\xff", "n"));
}

TEST(MleCodec, CallRoundTrip) {
  CallMsg m;
  m.routine_id = 7;
  m.library_epoch = 99;
  m.args = all_kinds_of_values();
  EXPECT_EQ(message_round_trip(m), m);
}

TEST(MleCodec, ResultRoundTrip) {
  ResultMsg m;
  m.ret = Value::vector({0.25f, 0.5f});
  m.out_params = {{1, Value::u64(3)}, {2, Value::decimal("1.50")}};
  m.console_out = "hello\n";
  m.console_cleared = true;
  m.console_err = std::string("\xff binary from a guest", 21);
  m.findings = {"name 'x' is never defined", "caf\xc3\xa9"};
  EXPECT_EQ(message_round_trip(m), m);

  ResultMsg e;
  e.error = Error{"js",
                  "TypeError: x is not a function",
                  {{"f", "routine.js", 3, 7, 12, "x()"},
                   {":program", "routine.js", 9, 1, 0, ""}},
                  0,
                  "",
                  0};
  EXPECT_EQ(message_round_trip(e), e);
  e.error->sql_errno = 1146;
  e.error->sqlstate = "42S02";
  e.error->sql_error_id = 0xffffffffffffffff;
  EXPECT_EQ(message_round_trip(e), e);
}

// The caps of §7.4 and §9.4: at the limit accepted, one beyond malformed.
TEST(MleCodec, ResultCaps) {
  auto with_error = [](auto change) {
    ResultMsg m;
    m.error = Error{"js", "x", {{"f", "r.js", 1, 1, 1, "t"}}, 0, "", 0};
    change(*m.error);
    return decodes(m);
  };
  EXPECT_TRUE(with_error([](Error &e) { e.message.assign(16384, 'm'); }));
  EXPECT_FALSE(with_error([](Error &e) { e.message.assign(16385, 'm'); }));
  EXPECT_TRUE(with_error([](Error &e) { e.stack[0].function.assign(1024, 'f'); }));
  EXPECT_FALSE(with_error([](Error &e) { e.stack[0].function.assign(1025, 'f'); }));
  EXPECT_TRUE(with_error([](Error &e) { e.stack[0].file.assign(1024, 'f'); }));
  EXPECT_FALSE(with_error([](Error &e) { e.stack[0].file.assign(1025, 'f'); }));
  EXPECT_TRUE(with_error([](Error &e) { e.stack[0].text.assign(1024, 't'); }));
  EXPECT_FALSE(with_error([](Error &e) { e.stack[0].text.assign(1025, 't'); }));
  EXPECT_TRUE(with_error([](Error &e) { e.stack.resize(1000); }));
  EXPECT_FALSE(with_error([](Error &e) { e.stack.resize(1001); }));
  EXPECT_TRUE(with_error([](Error &e) { e.sql_errno = 65535; }));
  EXPECT_FALSE(with_error([](Error &e) { e.sql_errno = 65536; }));
  EXPECT_TRUE(with_error([](Error &e) { e.sqlstate = "HY000"; }));
  EXPECT_FALSE(with_error([](Error &e) { e.sqlstate = "hy000"; }));
  EXPECT_FALSE(with_error([](Error &e) { e.sqlstate = "HY00"; }));
  EXPECT_FALSE(with_error([](Error &e) { e.message = "\xff"; }));
  EXPECT_FALSE(with_error([](Error &e) { e.language = "\xff"; }));
  EXPECT_FALSE(with_error([](Error &e) { e.stack[0].text = "\xff"; }));

  auto with_findings = [](size_t count, size_t length) {
    ResultMsg m;
    m.findings.assign(count, std::string(length, 'f'));
    return decodes(m);
  };
  EXPECT_TRUE(with_findings(1000, 1));
  EXPECT_FALSE(with_findings(1001, 1));
  EXPECT_TRUE(with_findings(1, 4096));
  EXPECT_FALSE(with_findings(1, 4097));
  ResultMsg bad_finding;
  bad_finding.findings = {"\xff"};
  EXPECT_FALSE(decodes(bad_finding));

  ResultMsg outs;
  outs.out_params.assign(65535, {0, Value::null()});
  EXPECT_TRUE(decodes(outs));
  outs.out_params.assign(65536, {0, Value::null()});
  EXPECT_FALSE(decodes(outs));
}

TEST(MleCodec, SqlRoundTrip) {
  SqlMsg m;
  m.action = wire::SqlAction::kExecute;
  m.stmt_id = 77;
  m.statement = "SELECT id, DISTANCE(v, ?, 'COSINE') FROM t ORDER BY 2 LIMIT ?";
  m.params = std::vector<Value>{Value::vector({1, 2, 3}), Value::i64(10)};
  m.flags = wire::kPassResultToClient;
  m.max_rows = 1000;
  EXPECT_EQ(message_round_trip(m), m);
  SqlMsg prepare;
  prepare.action = wire::SqlAction::kPrepare;
  prepare.stmt_id = 1;
  prepare.statement = "INSERT INTO t VALUES (?, ?)";
  prepare.flags = wire::kPassResultToClient;
  EXPECT_EQ(message_round_trip(prepare), prepare);
  SqlMsg execute;
  execute.action = wire::SqlAction::kExecutePrepared;
  execute.stmt_id = 1;
  execute.params = {Value::i64(1), Value::null()};
  execute.max_rows = 10;
  EXPECT_EQ(message_round_trip(execute), execute);
  SqlMsg fetch;
  fetch.action = wire::SqlAction::kFetch;
  fetch.stmt_id = 77;
  fetch.max_rows = 5;
  EXPECT_EQ(message_round_trip(fetch), fetch);
  SqlMsg next;
  next.action = wire::SqlAction::kNextResult;
  next.stmt_id = 77;
  EXPECT_EQ(message_round_trip(next), next);
  SqlMsg close;
  close.action = wire::SqlAction::kClose;
  close.stmt_id = 0xffffffffffffffff;
  EXPECT_EQ(message_round_trip(close), close);
  std::string bytes;
  encode(close, &bytes);
  EXPECT_EQ(bytes.size(), wire::kSqlCloseSize);  // always 26 bytes (§5.3)

  // Rows of row_width values one after another. Whether they are whole
  // rows is not the codec's question: it is an error, not a violation.
  SqlMsg many;
  many.action = wire::SqlAction::kExecuteMany;
  many.stmt_id = 78;
  many.params = std::vector<Value>{Value::i64(1), Value::vector({1, 2}),
                                   Value::i64(2), Value::null()};
  many.row_width = 2;
  EXPECT_EQ(message_round_trip(many), many);
  many.row_width = 3;
  EXPECT_EQ(message_round_trip(many), many);
  many.row_width = 0;
  EXPECT_EQ(message_round_trip(many), many);
}

// An unknown action or flag, and a field the action does not use (§10.2).
TEST(MleCodec, SqlUnusedFields) {
  using wire::SqlAction;
  auto bad = [](SqlAction action, auto change) {
    SqlMsg m;
    m.action = action;
    m.stmt_id = 1;
    EXPECT_TRUE(decodes(m)) << int(action);
    change(m);
    return !decodes(m);
  };
  auto text = [](SqlMsg &m) { m.statement = "SELECT 1"; };
  auto params = [](SqlMsg &m) { m.params = {Value::i64(1)}; };
  auto flags = [](SqlMsg &m) { m.flags = wire::kPassResultToClient; };
  auto max_rows = [](SqlMsg &m) { m.max_rows = 1; };
  auto row_width = [](SqlMsg &m) { m.row_width = 1; };
  EXPECT_FALSE(bad(SqlAction::kExecute, text));
  EXPECT_FALSE(bad(SqlAction::kExecute, params));
  EXPECT_FALSE(bad(SqlAction::kExecute, flags));
  EXPECT_FALSE(bad(SqlAction::kExecute, max_rows));
  EXPECT_TRUE(bad(SqlAction::kExecute, row_width));
  EXPECT_FALSE(bad(SqlAction::kPrepare, text));
  EXPECT_FALSE(bad(SqlAction::kPrepare, flags));
  EXPECT_TRUE(bad(SqlAction::kPrepare, params));
  EXPECT_TRUE(bad(SqlAction::kPrepare, max_rows));
  EXPECT_TRUE(bad(SqlAction::kPrepare, row_width));
  EXPECT_FALSE(bad(SqlAction::kExecutePrepared, params));
  EXPECT_FALSE(bad(SqlAction::kExecutePrepared, max_rows));
  EXPECT_TRUE(bad(SqlAction::kExecutePrepared, text));
  EXPECT_TRUE(bad(SqlAction::kExecutePrepared, flags));
  EXPECT_TRUE(bad(SqlAction::kExecutePrepared, row_width));
  for (const SqlAction a : {SqlAction::kFetch, SqlAction::kNextResult}) {
    EXPECT_FALSE(bad(a, max_rows));
    EXPECT_TRUE(bad(a, text));
    EXPECT_TRUE(bad(a, params));
    EXPECT_TRUE(bad(a, flags));
    EXPECT_TRUE(bad(a, row_width));
  }
  EXPECT_TRUE(bad(SqlAction::kClose, text));
  EXPECT_TRUE(bad(SqlAction::kClose, params));
  EXPECT_TRUE(bad(SqlAction::kClose, flags));
  EXPECT_TRUE(bad(SqlAction::kClose, max_rows));
  EXPECT_TRUE(bad(SqlAction::kClose, row_width));
  EXPECT_FALSE(bad(SqlAction::kExecuteMany, params));
  EXPECT_FALSE(bad(SqlAction::kExecuteMany, row_width));
  EXPECT_TRUE(bad(SqlAction::kExecuteMany, text));
  EXPECT_TRUE(bad(SqlAction::kExecuteMany, flags));
  EXPECT_TRUE(bad(SqlAction::kExecuteMany, max_rows));

  SqlMsg m;
  m.statement = "SELECT 1";
  std::string bytes;
  encode(m, &bytes);
  SqlMsg back;
  bytes[0] = 7;  // no such action
  EXPECT_FALSE(decode(bytes, &back, nullptr));
  encode(m, &bytes);
  bytes[bytes.size() - 9] = 2;  // unknown flag
  EXPECT_FALSE(decode(bytes, &back, nullptr));
  // A text that is not well-formed is never repaired: it is malformed.
  m.statement = "SELECT '\xff'";
  EXPECT_FALSE(decodes(m));
  m.statement = "SELECT 1";
  m.params.assign(65535, Value::null());
  EXPECT_TRUE(decodes(m));
  m.params.assign(65536, Value::null());
  EXPECT_FALSE(decodes(m));
}

TEST(MleCodec, SqlResultRoundTrip) {
  SqlResultMsg err;
  err.kind = SqlResultKind::kError;
  err.sql_errno = 1062;
  err.sqlstate = "23000";
  err.message = "Duplicate entry '1' for key 't.PRIMARY'";
  err.error_id = 3;
  EXPECT_EQ(message_round_trip(err), err);
  err.failed_row = 41;
  EXPECT_EQ(message_round_trip(err), err);

  SqlResultMsg ok;
  ok.kind = SqlResultKind::kOk;
  ok.affected_rows = 3;
  ok.insert_id = 42;
  ok.warnings = {{1, 1265, "Data truncated for column 'c' at row 1"}};
  ok.more_results = true;
  ok.exec_us = 1234;
  EXPECT_EQ(message_round_trip(ok), ok);
  SqlResultMsg prepared;
  prepared.kind = SqlResultKind::kOk;
  prepared.param_count = 65535;
  EXPECT_EQ(message_round_trip(prepared), prepared);

  // The first batch of a set carries its columns.
  SqlResultMsg rows;
  rows.kind = SqlResultKind::kRows;
  ParamType id;
  id.tag = Tag::kU64;
  id.is_unsigned = true;
  id.type_name = "BIGINT";
  ParamType d;
  d.tag = Tag::kF32;
  d.type_name = "FLOAT";
  Column full("id", "t", id);
  full.org_name = "id";
  full.schema = "test";
  full.org_table = "points";
  full.charset = "binary";
  full.collation = "binary";
  full.flags = 32;
  rows.columns = {full, {"dist", "", d}};
  for (uint64_t i = 0; i < 100; i++) {
    rows.rows.push_back(Value::u64(i));
    rows.rows.push_back(i % 7 == 0 ? Value::null() : Value::f32(1.0f / (i + 1)));
  }
  rows.total_rows = 5000;
  rows.more = true;
  rows.exec_us = 99;
  EXPECT_EQ(message_round_trip(rows), rows);

  // A later batch carries none: the receiver knows the set's column count.
  SqlResultMsg later = rows;
  later.columns.clear();
  later.ncolumns = 2;
  later.more = false;
  later.more_results = true;
  later.warnings = {{0, 1003, "note"}, {2, 1048, "error"}};
  std::string bytes, error;
  encode(later, &bytes);
  SqlResultMsg back;
  ASSERT_TRUE(decode(bytes, &back, &error, 2)) << error;
  EXPECT_EQ(back, later);
  EXPECT_FALSE(decode(bytes, &back, &error, 3));
  EXPECT_FALSE(decode(bytes, &back, &error));  // rows without columns

  // An empty set: its one batch has the columns and no rows; the host may
  // not know the number of rows.
  SqlResultMsg empty;
  empty.kind = SqlResultKind::kRows;
  empty.columns = rows.columns;
  EXPECT_EQ(empty.total_rows, wire::kUnknownTotalRows);
  EXPECT_EQ(message_round_trip(empty), empty);
  encode(empty, &bytes);
  EXPECT_NE(bytes.find(std::string(8, '\xff')), std::string::npos);

  SqlResultMsg end;
  end.kind = SqlResultKind::kEnd;
  EXPECT_EQ(message_round_trip(end), end);
  encode(end, &bytes);
  EXPECT_EQ(bytes, std::string(1, '\x03'));
}

// A result's rows written in parts straight into a fixed buffer (the
// arena) by the typed writers, as the component does, give the bytes
// encode(SqlResultMsg) gives.
void put_typed(Writer &w, const Value &v) {
  switch (v.tag()) {
    case Tag::kNull:
      return w.null_value();
    case Tag::kI64:
      return w.i64_value(v.as_i64());
    case Tag::kU64:
      return w.u64_value(v.as_u64());
    case Tag::kF64:
      return w.f64_value(v.as_f64());
    case Tag::kF32:
      return w.f32_value(v.as_f32());
    case Tag::kDecimal:
    case Tag::kString:
    case Tag::kBytes:
    case Tag::kJson:
      return w.text_value(v.tag(), v.as_text());
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
      return w.temporal_value(v.tag(), v.as_temporal());
    case Tag::kTimestamp:
      return w.timestamp_value(v.as_timestamp());
    case Tag::kVector:
      return w.vector_value(v.as_vector().data(),
                            static_cast<uint32_t>(v.as_vector().size()));
  }
}

TEST(MleCodec, RowsWrittenInPartsMatchEncode) {
  const std::vector<Value> values = all_kinds_of_values();
  SqlResultMsg m;
  m.kind = SqlResultKind::kRows;
  for (size_t i = 0; i < values.size(); i++) {
    ParamType t;
    t.tag = values[i].tag() == Tag::kNull ? Tag::kString : values[i].tag();
    m.columns.push_back(Column("c" + std::to_string(i), "t", t));
  }
  // Two rows; the second shifted by one, so its vectors start at other
  // offsets (their padding differs).
  for (const Value &v : values) m.rows.push_back(v);
  for (size_t i = 0; i < values.size(); i++)
    m.rows.push_back(values[(i + 1) % values.size()]);
  m.total_rows = 2;
  m.more = true;
  m.warnings = {{1, 1265, "Data truncated for column 'c' at row 1"}};
  m.exec_us = 7;
  std::string expected;
  encode(m, &expected);

  std::vector<char> buf(expected.size());
  Writer w(buf.data(), buf.size());
  const size_t count_at = put_rows_head(w, m.columns, m.total_rows);
  for (const Value &v : m.rows) put_typed(w, v);
  w.patch_u32(count_at, 2);
  put_rows_tail(w, m.more, m.more_results, m.warnings, m.exec_us);
  ASSERT_FALSE(w.overflow());
  ASSERT_EQ(w.position(), expected.size());
  EXPECT_EQ(std::string(buf.data(), w.position()), expected);

  SqlResultMsg back;
  std::string error;
  ASSERT_TRUE(decode(expected, &back, &error)) << error;
  EXPECT_EQ(back, m);
}

TEST(MleCodec, FixedBufferOverflowRollsBack) {
  char buf[64];
  Writer w(buf, sizeof buf);
  w.u32(7);
  const size_t at = w.position();
  const std::vector<float> big(100, 1.0f);  // 400 bytes: does not fit
  w.vector_value(big.data(), 100);
  EXPECT_TRUE(w.overflow());
  w.u8(1);  // still overflowing until rolled back
  EXPECT_TRUE(w.overflow());
  w.set_position(at);
  EXPECT_FALSE(w.overflow());
  EXPECT_EQ(w.position(), at);
  const std::vector<float> small(4, 2.0f);
  w.vector_value(small.data(), 4);
  ASSERT_FALSE(w.overflow());
  // u32 7, then the vector: tag, dims, padding to 16, 16 bytes of floats.
  EXPECT_EQ(w.position(), 4 + 1 + 4 + 7 + 16);
  float back[4];
  memcpy(back, buf + 16, sizeof back);
  EXPECT_EQ(back[3], 2.0f);
}

TEST(MleCodec, SmallMessagesRoundTrip) {
  EXPECT_EQ(message_round_trip(DropMsg{99}), DropMsg{99});
}

TEST(MleCodec, OptionsRoundTrip) {
  OptionsMsg m;
  EXPECT_EQ(message_round_trip(m), m);
  m.time_zone = "Europe/Kyiv";
  m.settings = {{"decimal_type", "number"}, {"integer_type", "bigint"}};
  EXPECT_EQ(message_round_trip(m), m);
  // time_zone, then the list of (name, value).
  std::string bytes;
  encode(m, &bytes);
  EXPECT_EQ(bytes.substr(0, 4 + 11 + 4 + 4 + 12),
            std::string("\x0b\0\0\0Europe/Kyiv\x02\0\0\0\x0c\0\0\0"
                        "decimal_type",
                        35));
}

TEST(MleCodec, OptionsDomains) {
  auto bad = [](auto change) {
    OptionsMsg m;
    m.settings = {{"a", "1"}, {"b", "2"}};
    change(m);
    std::string why;
    const bool refused = !is_valid(m, &why);
    EXPECT_EQ(refused, !decodes(m)) << why;
    return refused;
  };
  EXPECT_FALSE(bad([](OptionsMsg &) {}));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.time_zone = ""; }));
  EXPECT_FALSE(bad([](OptionsMsg &m) { m.time_zone.assign(64, 'z'); }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.time_zone.assign(65, 'z'); }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"b", "1"}, {"a", "2"}}; }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"a", "1"}, {"a", "2"}}; }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"", "1"}}; }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"A", "1"}}; }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"a b", "1"}}; }));
  EXPECT_FALSE(bad([](OptionsMsg &m) {
    m.settings = {{std::string(64, 'n'), std::string(1024, 'v')}};
  }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{std::string(65, 'n'), ""}}; }));
  EXPECT_TRUE(bad([](OptionsMsg &m) { m.settings = {{"a", std::string(1025, 'v')}}; }));
  auto many = [](int count) {
    return [count](OptionsMsg &m) {
      m.settings.clear();
      for (int i = 0; i < count; i++) {
        std::string digits = std::to_string(i);
        digits.insert(0, 3 - digits.size(), '0');
        m.settings.push_back({"s" + digits, ""});
      }
    };
  };
  EXPECT_FALSE(bad(many(256)));
  EXPECT_TRUE(bad(many(257)));
}

TEST(MleCodec, BadMessagesAreRejected) {
  std::string error;
  {  // bad parameter mode
    PrepareMsg m;
    m.params = {{"a", ParamMode::kIn, {}}};
    std::string bytes;
    encode(m, &bytes);
    const size_t mode_at = 8 + 4 * 4 + 4 + 4 + 1;  // id, 4 empty strs, n, "a"
    ASSERT_EQ(bytes[mode_at], 0);
    bytes[mode_at] = 3;
    PrepareMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
  }
  {  // a bool that is neither 0 nor 1
    ResultMsg m;
    std::string bytes;
    encode(m, &bytes);
    bytes[0] = 2;
    ResultMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
    EXPECT_NE(error.find("boolean"), std::string::npos) << error;
  }
  {  // a count that the bytes cannot hold must fail before allocating
    std::string bytes;
    Writer w(&bytes);
    w.u8(0);  // EXECUTE
    w.u64(1);
    w.str("SELECT 1");
    w.u32(0xFFFFFFFF);  // parameters
    SqlMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
    EXPECT_NE(error.find("count"), std::string::npos) << error;
  }
  {  // rows x columns overflow
    std::string bytes;
    Writer w(&bytes);
    w.u8(uint8_t(SqlResultKind::kRows));
    w.u32(1);
    w.str("c");
    w.str("");
    w.u8(uint8_t(Tag::kI64));
    w.u8(0);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    for (int i = 0; i < 6; i++) w.str("");  // type name .. collation
    w.u32(0);                               // flags
    w.u64(1);           // total rows
    w.u32(0xFFFFFFFF);  // rows
    SqlResultMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
  }
  {  // bad SQL result kind
    SqlResultMsg back;
    EXPECT_FALSE(decode(std::string(1, '\x04'), &back, &error));
  }
  {  // more columns than a result set may have
    SqlResultMsg m;
    m.kind = SqlResultKind::kRows;
    m.columns.resize(4096);
    EXPECT_TRUE(decodes(m));
    m.columns.resize(4097);
    EXPECT_FALSE(decodes(m));
  }
}

// ---------------------------------------------------------------- chunks

MessageHeader header(uint32_t len, uint64_t total, bool more,
                     uint32_t op = uint32_t(wire::Op::kResult)) {
  MessageHeader h{};
  h.op = op;
  h.len = len;
  h.total_len = total;
  h.flags = more ? uint32_t{wire::kMore} : 0U;
  h.depth = 1;
  h.call_seq = 77;
  return h;
}

TEST(MleAssembler, SingleChunk) {
  Assembler a(1024, wire::kMaxWorkerMessage);
  EXPECT_EQ(a.add(header(5, 5, false), "hello"), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), "hello");
  EXPECT_EQ(a.header().call_seq, 77u);
  a.reset();
  EXPECT_EQ(a.add(header(0, 0, false), ""), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), "");
  a.reset();
  // A message of exactly the arena is one chunk.
  const std::string full(1024, 'f');
  EXPECT_EQ(a.add(header(1024, 1024, false), full.data()),
            Assembler::Step::kDone);
}

TEST(MleAssembler, ManyChunks) {
  const uint32_t arena = 64 * 1024;
  std::string message(5 * arena + 123, '\0');
  for (size_t i = 0; i < message.size(); i++) message[i] = char(i * 31);
  Assembler a(arena, wire::kMaxWorkerMessage);
  size_t sent = 0;
  Assembler::Step step = Assembler::Step::kNeedMore;
  while (sent < message.size()) {
    const uint32_t len =
        uint32_t(std::min<size_t>(arena, message.size() - sent));
    const bool more = sent + len < message.size();
    step = a.add(header(len, message.size(), more), message.data() + sent);
    sent += len;
    ASSERT_EQ(step, more ? Assembler::Step::kNeedMore : Assembler::Step::kDone)
        << a.error();
    EXPECT_EQ(a.received(), sent);
  }
  EXPECT_EQ(a.body(), message);
}

// A message that is discarded (§5.3): its chunks are checked and counted,
// and only its first bytes are kept.
TEST(MleAssembler, Discard) {
  const uint32_t arena = 64;
  const std::string data(arena, 'd');
  Assembler a(arena, 1000);
  ASSERT_EQ(a.add(header(64, 150, true), data.data()),
            Assembler::Step::kNeedMore);
  a.discard_beyond(9);
  EXPECT_TRUE(a.discarding());
  EXPECT_EQ(a.body(), std::string(9, 'd'));
  ASSERT_EQ(a.add(header(64, 150, true), data.data()),
            Assembler::Step::kNeedMore);
  ASSERT_EQ(a.add(header(22, 150, false), data.data()), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), std::string(9, 'd'));
  EXPECT_EQ(a.received(), 150u);
  // The rules hold for what is not kept.
  a.reset();
  EXPECT_FALSE(a.discarding());
  ASSERT_EQ(a.add(header(64, 150, true), data.data()),
            Assembler::Step::kNeedMore);
  a.discard_beyond(9);
  EXPECT_EQ(a.add(header(30, 150, false), data.data()), Assembler::Step::kError);
}

TEST(MleAssembler, BadChunksAreRejected) {
  const std::string data(100, 'x');
  struct Case {
    const char *what;
    std::vector<MessageHeader> chunks;
  };
  MessageHeader other_op = header(36, 100, false, uint32_t(wire::Op::kSql));
  MessageHeader other_seq = header(36, 100, false);
  other_seq.call_seq = 78;
  MessageHeader other_depth = header(36, 100, false);
  other_depth.depth = 2;
  MessageHeader other_status = header(36, 100, false);
  other_status.status = 1;
  MessageHeader other_total = header(36, 101, false);
  const std::vector<Case> cases = {
      {"chunk larger than the arena", {header(65, 65, false)}},
      {"message too large", {header(64, 1001, true)}},
      {"chunk larger than the message", {header(20, 10, false)}},
      {"short single chunk", {header(5, 10, false)}},
      {"a whole message with more", {header(64, 64, true)}},
      {"empty chunk with more", {header(0, 10, true)}},
      {"a chunk before the last that is not full", {header(10, 100, true)}},
      {"overrun", {header(64, 100, true), header(37, 100, false)}},
      {"truncated", {header(64, 100, true), header(35, 100, false)}},
      {"empty last chunk", {header(64, 100, true), header(0, 100, false)}},
      {"more after the last byte",
       {header(64, 128, true), header(64, 128, true)}},
      {"op changes", {header(64, 100, true), other_op}},
      {"status changes", {header(64, 100, true), other_status}},
      {"call_seq changes", {header(64, 100, true), other_seq}},
      {"depth changes", {header(64, 100, true), other_depth}},
      {"total changes", {header(64, 100, true), other_total}},
  };
  for (const Case &c : cases) {
    Assembler a(64, 1000);
    Assembler::Step step = Assembler::Step::kNeedMore;
    for (const MessageHeader &h : c.chunks) step = a.add(h, data.data());
    EXPECT_EQ(step, Assembler::Step::kError) << c.what;
    EXPECT_FALSE(a.error().empty()) << c.what;
    // An assembler in error stays in error.
    EXPECT_EQ(a.add(header(1, 1, false), "x"), Assembler::Step::kError);
  }
}

// All chunks of a RESULT carry the same TAINTED flag (§5.2).
TEST(MleAssembler, TaintedInEveryChunk) {
  const std::string data(64, 'x');
  for (const bool tainted : {false, true}) {
    for (const bool agrees : {false, true}) {
      Assembler a(64, wire::kMaxWorkerMessage);
      MessageHeader first = header(64, 100, true);
      MessageHeader last = header(36, 100, false);
      if (tainted) first.flags |= wire::kTainted;
      if (tainted == agrees) last.flags |= wire::kTainted;
      ASSERT_EQ(a.add(first, data.data()), Assembler::Step::kNeedMore);
      EXPECT_EQ(a.add(last, data.data()),
                agrees ? Assembler::Step::kDone : Assembler::Step::kError);
      if (agrees)
        EXPECT_EQ((a.header().flags & wire::kTainted) != 0, tainted);
    }
  }
}

// What the header alone tells (§13).
TEST(MleAssembler, WorkerHeaderCheck) {
  std::string error;
  auto bad = [&error](auto change, uint32_t op = uint32_t(wire::Op::kResult)) {
    MessageHeader h = header(10, 10, false, op);
    change(h);
    return !check_worker_header(h, 1024, &error);
  };
  EXPECT_FALSE(bad([](MessageHeader &) {}));
  // op: assigned.
  for (uint32_t op : {0u, 7u, 15u, 0xFFFFFFFFu})
    EXPECT_TRUE(bad([op](MessageHeader &h) { h.op = op; })) << op;
  // status: 0 unless RESULT (0 to 4) or READY (0 or 4).
  for (uint32_t s = 0; s <= 4; s++)
    EXPECT_FALSE(bad([s](MessageHeader &h) { h.status = s; })) << s;
  EXPECT_TRUE(bad([](MessageHeader &h) { h.status = 5; }));
  EXPECT_TRUE(bad([](MessageHeader &h) { h.status = 0x80000000; }));
  const auto ready = uint32_t(wire::Op::kReady);
  EXPECT_FALSE(bad([](MessageHeader &h) { h.status = 4; }, ready));
  for (uint32_t s : {1u, 2u, 3u, 5u})
    EXPECT_TRUE(bad([s](MessageHeader &h) { h.status = s; }, ready)) << s;
  for (const wire::Op op : {wire::Op::kSql, wire::Op::kLibrary}) {
    EXPECT_FALSE(bad([](MessageHeader &) {}, uint32_t(op)));
    EXPECT_TRUE(bad([](MessageHeader &h) { h.status = 1; }, uint32_t(op)));
    EXPECT_TRUE(bad([](MessageHeader &h) { h.status = 4; }, uint32_t(op)));
  }
  // flags: only MORE and TAINTED, MORE only with a full arena, TAINTED only
  // in a RESULT.
  EXPECT_FALSE(bad([](MessageHeader &h) { h.flags = wire::kTainted; }));
  EXPECT_FALSE(bad([](MessageHeader &h) {
    h.flags = wire::kMore | wire::kTainted, h.len = 1024, h.total_len = 5000;
  }));
  for (const wire::Op op : {wire::Op::kReady, wire::Op::kSql,
                            wire::Op::kLibrary, wire::Op::kContinue})
    EXPECT_TRUE(bad([](MessageHeader &h) { h.flags = wire::kTainted; },
                    uint32_t(op)))
        << uint32_t(op);
  EXPECT_TRUE(bad([](MessageHeader &h) { h.flags = 4; }));
  EXPECT_TRUE(bad([](MessageHeader &h) { h.flags = 0x80000001; }));
  EXPECT_TRUE(bad([](MessageHeader &h) { h.flags = 1, h.total_len = 5000; }));
  EXPECT_FALSE(bad([](MessageHeader &h) {
    h.flags = 1, h.len = 1024, h.total_len = 5000;
  }));
  // reserved: 0.
  EXPECT_TRUE(bad([](MessageHeader &h) { h.reserved = 1; }));
  // len and total_len.
  EXPECT_TRUE(bad([](MessageHeader &h) { h.len = 1025, h.total_len = 1025; }));
  EXPECT_FALSE(bad([](MessageHeader &h) {
    h.flags = 1, h.len = 1024, h.total_len = wire::kMaxWorkerMessage;
  }));
  EXPECT_TRUE(bad([](MessageHeader &h) {
    h.flags = 1, h.len = 1024, h.total_len = wire::kMaxWorkerMessage + 1;
  }));
  // CONTINUE: every field.
  const auto go = uint32_t(wire::Op::kContinue);
  auto empty = [](MessageHeader &h) { h.len = 0, h.total_len = 0; };
  EXPECT_FALSE(bad(empty, go));
  EXPECT_TRUE(bad([](MessageHeader &h) { h.len = 0, h.total_len = 1; }, go));
  EXPECT_TRUE(bad([](MessageHeader &h) { h.len = 1, h.total_len = 0; }, go));
  EXPECT_TRUE(bad([](MessageHeader &h) {
    h.len = 0, h.total_len = 0, h.status = 1;
  }, go));
  EXPECT_TRUE(bad([](MessageHeader &h) {
    h.len = 0, h.total_len = 0, h.reserved = 1;
  }, go));
  EXPECT_TRUE(bad([](MessageHeader &h) {
    h.len = 0, h.total_len = 0, h.flags = 1;
  }, go));
}

// §1 region rules: a decoder reads each byte once and checks a field only
// after it copied it, so a worker that changes the region while the host
// decodes cannot make the host accept what it checked as something else.
// A byte of a string flips between a valid and an invalid UTF-8 lead byte
// on another thread: a decoder that validated the region and then copied it
// would now and then keep an invalid string.
TEST(MleRegionRules, FieldsAreCopiedBeforeTheyAreChecked) {
  ResultMsg m;
  m.ret = Value::string(std::string(64, 'a'));
  m.error = Error{"", std::string(64, 'b'), {}, 0, "", 0};
  m.console_out = "out";
  std::string body;
  encode(m, &body);
  // The region: decoded where it is, as the host does with a RESULT.
  std::vector<char> region(body.begin(), body.end());
  const size_t ret_at = 1 + 1 + 4 + 32;  // a byte inside the returned string
  const size_t message_at = body.rfind(std::string(64, 'b')) + 32;
  std::atomic<bool> stop{false};
  std::thread flipper([&] {
    auto *r = reinterpret_cast<volatile char *>(region.data());
    while (!stop.load(std::memory_order_relaxed)) {
      r[ret_at] = '\xff';
      r[message_at] = '\xff';
      r[ret_at] = 'a';
      r[message_at] = 'b';
    }
  });
  int accepted = 0;
  for (int i = 0; i < 200000; i++) {
    ResultMsg got;
    std::string error;
    if (!decode(std::string_view(region.data(), region.size()), &got, &error))
      continue;
    ++accepted;
    ASSERT_TRUE(got.ret && is_valid_utf8(got.ret->as_text()));
    ASSERT_TRUE(got.error && is_valid_utf8(got.error->message));
  }
  stop = true;
  flipper.join();
  EXPECT_GT(accepted, 0);
}

// The SQL message decoded in two parts (head read by the receiver, the rest
// where it is) is the message decoded whole; offsets count from the body.
TEST(MleRegionRules, SqlRestIsTheWholeMessage) {
  SqlMsg m;
  m.action = wire::SqlAction::kExecute;
  m.stmt_id = 77;
  m.statement = "SELECT ?, ?";
  m.params = {Value::vector({1.0f, 2.0f}), Value::string("x")};
  m.max_rows = 5;
  std::string body;
  encode(m, &body);
  SqlMsg rest;
  rest.action = m.action;
  rest.stmt_id = m.stmt_id;
  std::string error;
  ASSERT_TRUE(decode_sql_rest(std::string_view(body).substr(wire::kSqlHeadSize),
                              &rest, &error))
      << error;
  EXPECT_EQ(rest, m);
  // A bad byte is reported at its offset in the body.
  std::string bad = body;
  const size_t text_at = wire::kSqlHeadSize + 4;
  bad[text_at] = '\xff';
  EXPECT_FALSE(decode_sql_rest(std::string_view(bad).substr(wire::kSqlHeadSize),
                               &rest, &error));
  SqlMsg whole;
  std::string whole_error;
  EXPECT_FALSE(decode(bad, &whole, &whole_error));
  EXPECT_EQ(error, whole_error);
  // A field the action does not use.
  rest.action = wire::SqlAction::kFetch;
  EXPECT_FALSE(decode_sql_rest(std::string_view(body).substr(wire::kSqlHeadSize),
                               &rest, &error));
  EXPECT_NE(error.find("does not use"), std::string::npos) << error;
}

// A message of one chunk can be checked without being copied.
TEST(MleAssembler, LeftInPlace) {
  Assembler a(1024, 4096);
  MessageHeader h{};
  h.op = uint32_t(wire::Op::kResult);
  h.len = 10;
  h.total_len = 10;
  const char chunk[10] = {};
  EXPECT_EQ(a.add(h, chunk, false), Assembler::Step::kDone);
  EXPECT_TRUE(a.body().empty());
  Assembler b(1024, 4096);
  h.flags = wire::kMore;
  h.len = 1024;
  h.total_len = 2000;
  std::vector<char> big(1024);
  EXPECT_EQ(b.add(h, big.data(), false), Assembler::Step::kError);
}

}  // namespace mle_codec_unittest
