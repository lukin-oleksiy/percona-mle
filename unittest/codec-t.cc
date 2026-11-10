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

// Unit tests of the MLE wire protocol codec (components/mle).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
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
                  std::string *error = nullptr) {
  Reader r(bytes);
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
      Value::decimal("-12345678901234567890123456789012345.123456789012345678"
                     "901234567890"),
      Value::string(""),
      Value::string("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"),  // é € 😀
      Value::bytes(std::string("a\0b\xff\xfe", 5)),
      Value::bytes(std::string(70000, '\x80')),
      Value::temporal(Tag::kDate, date(2026, 11, 10)),
      Value::temporal(Tag::kDate, date(0, 0, 0)),
      Value::temporal(Tag::kDatetime,
                      datetime(9999, 12, 31, 23, 59, 59, 999999)),
      Value::temporal(Tag::kTimestamp, datetime(1970, 1, 1, 0, 0, 1, 0)),
      Value::temporal(Tag::kTime, time_of(true, 838, 59, 59, 0)),
      Value::temporal(Tag::kTime, time_of(false, 0, 0, 0, 1)),
      Value::json(R"({"a":[1,2,{"b":null}],"c":"é"})"),
      Value::vector({}),
      Value::vector({1.0f, -2.0f, std::numeric_limits<float>::quiet_NaN()}),
      Value::vector(v768),
      Value::vector(std::vector<float>(wire::kMaxVectorDimensions, 1.5f)),
  };
}

// ---------------------------------------------------------------- layout

TEST(MleProtocol, RegionLayout) {
  EXPECT_EQ(wire::region_size(wire::kDefaultArenaSize),
            wire::kHeaderSize + (1u << 20));
  EXPECT_EQ(offsetof(wire::RegionHeader, msg) + offsetof(MessageHeader, len),
            256u + 16u);
  EXPECT_EQ(wire::kMagic, uint32_t('M') | uint32_t('L') << 8 |
                              uint32_t('E') << 16 | uint32_t('1') << 24);
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
  EXPECT_NE(Value::temporal(Tag::kDatetime, date(2026, 1, 1)),
            Value::temporal(Tag::kTimestamp, date(2026, 1, 1)));
}

TEST(MleCodec, VectorDataIsAligned) {
  const Value v = Value::vector({1.0f, 2.0f, 3.0f});
  for (size_t prefix = 0; prefix < 40; prefix++) {
    std::string bytes = encode_value(v, prefix);
    // tag + u32 dimensions, then padding to 16 relative to the start.
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

TEST(MleCodec, BadValuesAreRejected) {
  std::string huge_vector;
  {
    Writer w(&huge_vector);
    w.u8(uint8_t(Tag::kVector));
    w.u32(wire::kMaxVectorDimensions + 1);
  }
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
      {"timestamp minute 60",
       temporal_bytes(Tag::kTimestamp, datetime(2026, 1, 1, 0, 60, 0, 0), 0)},
      {"datetime micro 1e6",
       temporal_bytes(Tag::kDatetime, datetime(2026, 1, 1, 0, 0, 0, 1000000),
                      0)},
      {"time hour 839",
       temporal_bytes(Tag::kTime, time_of(false, 839, 0, 0, 0), 0)},
      {"time beyond max",
       temporal_bytes(Tag::kTime, time_of(false, 838, 59, 59, 1), 0)},
      {"time with date",
       temporal_bytes(Tag::kTime, datetime(0, 0, 1, 1, 0, 0, 0), 0)},
      {"negative flag 2",
       temporal_bytes(Tag::kTime, time_of(false, 1, 0, 0, 0), 2)},
      {"vector too large", huge_vector},
      {"vector padding not zero", dirty_padding},
  };
  for (const BadValue &c : cases) {
    Value v;
    std::string error;
    EXPECT_FALSE(decode_value(c.bytes, &v, 0, &error)) << c.what;
    EXPECT_FALSE(error.empty()) << c.what;
  }
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

ParamType vector_type(uint32_t dim) {
  ParamType t;
  t.tag = Tag::kVector;
  t.length = dim;
  return t;
}

TEST(MleCodec, ReadyRoundTrip) {
  ReadyMsg m;
  m.worker_class = "jvm";
  m.languages = {"java", "js", "python"};
  m.runtime = "GraalVM CE 25.5.5+3";
  m.info = "ready in 92 ms";
  EXPECT_EQ(message_round_trip(m), m);
  EXPECT_EQ(message_round_trip(ReadyMsg()), ReadyMsg());
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
  m.params = {{"q", ParamMode::kIn, vector_type(768)},
              {"k", ParamMode::kInOut, u},
              {"total", ParamMode::kOut, dec}};
  m.returns = vector_type(3);
  m.imports = {{"jslib", "lib1", "mylib"}, {"db1", "theirlib", ""}};
  EXPECT_EQ(message_round_trip(m), m);
  m.returns.reset();
  m.params.clear();
  m.imports.clear();
  EXPECT_EQ(message_round_trip(m), m);
}

// Phase 6: CREATE LIBRARY's check and the libraries a routine imports.
TEST(MleCodec, LibraryMessagesRoundTrip) {
  ParseLibraryMsg p;
  p.language = "js";
  p.name = "lib1";
  p.body = "export function f(n) { return n }";
  EXPECT_EQ(message_round_trip(p), p);
  LibraryMsg l;
  l.schema = "db1";
  l.name = "theirlib";
  l.known_hash = 0x9a91d68a5f842a47;
  EXPECT_EQ(message_round_trip(l), l);
  LibraryResultMsg r;
  r.status = wire::LibraryStatus::kBody;
  r.language = "js";
  r.body = "import {foo} from \"/db1/mylib\"";
  r.hash = library_hash(r.body);
  EXPECT_EQ(message_round_trip(r), r);
  r.status = wire::LibraryStatus::kRefused;
  r.message = "library other.secret is not importable";
  EXPECT_EQ(message_round_trip(r), r);
  // A bad status is rejected.
  std::string bytes;
  encode(r, &bytes);
  bytes[0] = 7;
  LibraryResultMsg bad;
  EXPECT_FALSE(decode(bytes, &bad, nullptr));
  // The hash is never 0 (reserved for "no body").
  EXPECT_NE(library_hash(""), 0u);
  EXPECT_NE(library_hash("a"), library_hash("b"));
}

TEST(MleCodec, CallRoundTrip) {
  CallMsg m;
  m.routine_id = 7;
  m.events = wire::kStatementEnd | wire::kTransactionCommit;
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
  EXPECT_EQ(message_round_trip(m), m);

  ResultMsg e;
  e.error = Error{"js",
                  "TypeError: x is not a function",
                  {{"f", "routine.js", 3, 7, 12}, {":program", "routine.js", 9, 1, 0}},
                  0,
                  ""};
  EXPECT_EQ(message_round_trip(e), e);
  e.error->sql_errno = 1146;
  e.error->sqlstate = "42S02";
  EXPECT_EQ(message_round_trip(e), e);
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
  SqlMsg fetch;
  fetch.action = wire::SqlAction::kFetch;
  fetch.stmt_id = 77;
  EXPECT_EQ(message_round_trip(fetch), fetch);
  std::string bytes, error;
  encode(fetch, &bytes);
  bytes[0] = 6;  // no such action
  SqlMsg back;
  EXPECT_FALSE(decode(bytes, &back, &error));
  encode(fetch, &bytes);
  bytes[bytes.size() - 5] = 2;  // unknown flag
  EXPECT_FALSE(decode(bytes, &back, &error));
}

TEST(MleCodec, SqlResultRoundTrip) {
  SqlResultMsg err;
  err.kind = SqlResultKind::kError;
  err.sql_errno = 1062;
  err.sqlstate = "23000";
  err.message = "Duplicate entry '1' for key 't.PRIMARY'";
  EXPECT_EQ(message_round_trip(err), err);

  SqlResultMsg ok;
  ok.kind = SqlResultKind::kOk;
  ok.affected_rows = 3;
  ok.insert_id = 42;
  ok.warnings = {{1, 1265, "Data truncated for column 'c' at row 1"}};
  ok.more_results = true;
  ok.param_count = 2;
  ok.exec_us = 1234;
  EXPECT_EQ(message_round_trip(ok), ok);

  SqlResultMsg rows;
  rows.kind = SqlResultKind::kRows;
  ParamType id;
  id.tag = Tag::kU64;
  id.is_unsigned = true;
  ParamType d;
  d.tag = Tag::kF64;
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
    rows.rows.push_back(i % 7 == 0 ? Value::null() : Value::f64(1.0 / (i + 1)));
  }
  rows.more = true;
  rows.more_results = true;
  rows.exec_us = 99;
  EXPECT_EQ(message_round_trip(rows), rows);

  SqlResultMsg empty;
  empty.kind = SqlResultKind::kRows;
  empty.columns = rows.columns;
  EXPECT_EQ(message_round_trip(empty), empty);
}

TEST(MleCodec, SmallMessagesRoundTrip) {
  EXPECT_EQ(message_round_trip(DropMsg{99}), DropMsg{99});
}

TEST(MleCodec, OptionsRoundTrip) {
  OptionsMsg m;
  EXPECT_EQ(message_round_trip(m), m);
  m.integer_type = wire::IntegerType::kBigint;
  m.decimal_type = wire::DecimalType::kNumber;
  m.time_zone = "Europe/Kyiv";
  EXPECT_EQ(message_round_trip(m), m);
  std::string bytes, error;
  encode(m, &bytes);
  OptionsMsg back;
  bytes[0] = 4;  // no such integer type
  EXPECT_FALSE(decode(bytes, &back, &error));
  encode(m, &bytes);
  bytes[1] = 2;  // no such decimal type
  EXPECT_FALSE(decode(bytes, &back, &error));
}

TEST(MleCodec, BadMessagesAreRejected) {
  std::string error;
  {  // unknown CALL events
    CallMsg m;
    m.events = 1u << 31;
    std::string bytes;
    encode(m, &bytes);
    CallMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
  }
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
  {  // bad SQLSTATE in a guest error
    ResultMsg m;
    m.error = Error{"js", "x", {}, 1146, "42s02"};
    std::string bytes;
    encode(m, &bytes);
    ResultMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
    EXPECT_NE(error.find("SQLSTATE"), std::string::npos) << error;
  }
  {  // too many stack frames
    ResultMsg m;
    m.error = Error{"js", "x",
                    std::vector<StackFrame>(wire::kMaxStackFrames + 1), 0, ""};
    std::string bytes;
    encode(m, &bytes);
    ResultMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
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
    w.u32(0xFFFFFFFF);  // rows
    SqlResultMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
  }
  {  // bad SQL result kind
    SqlResultMsg back;
    EXPECT_FALSE(decode(std::string(1, '\x03'), &back, &error));
  }
  {  // bad READY language (invalid UTF-8)
    ReadyMsg m;
    m.languages = {"\xff"};
    std::string bytes;
    encode(m, &bytes);
    ReadyMsg back;
    EXPECT_FALSE(decode(bytes, &back, &error));
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
  Assembler a(1024, wire::kMaxMessageSize);
  EXPECT_EQ(a.add(header(5, 5, false), "hello"), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), "hello");
  EXPECT_EQ(a.header().call_seq, 77u);
  a.reset();
  EXPECT_EQ(a.add(header(0, 0, false), ""), Assembler::Step::kDone);
  EXPECT_EQ(a.body(), "");
}

TEST(MleAssembler, ManyChunks) {
  const uint32_t arena = 64 * 1024;
  std::string message(5 * arena + 123, '\0');
  for (size_t i = 0; i < message.size(); i++) message[i] = char(i * 31);
  Assembler a(arena, wire::kMaxMessageSize);
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
  }
  EXPECT_EQ(a.body(), message);
}

TEST(MleAssembler, BadChunksAreRejected) {
  const std::string data(100, 'x');
  struct Case {
    const char *what;
    std::vector<MessageHeader> chunks;
  };
  MessageHeader other_op = header(10, 20, false, uint32_t(wire::Op::kSql));
  MessageHeader other_seq = header(10, 20, false);
  other_seq.call_seq = 78;
  MessageHeader other_depth = header(10, 20, false);
  other_depth.depth = 2;
  MessageHeader other_total = header(10, 21, false);
  const std::vector<Case> cases = {
      {"chunk larger than the arena", {header(65, 65, false)}},
      {"message too large", {header(10, 1001, true)}},
      {"chunk larger than the message", {header(20, 10, false)}},
      {"short single chunk", {header(5, 10, false)}},
      {"empty chunk with more", {header(0, 10, true)}},
      {"more after the last byte", {header(10, 10, true)}},
      {"overrun", {header(10, 15, true), header(10, 15, false)}},
      {"truncated", {header(10, 30, true), header(10, 30, false)}},
      {"op changes", {header(10, 20, true), other_op}},
      {"call_seq changes", {header(10, 20, true), other_seq}},
      {"depth changes", {header(10, 20, true), other_depth}},
      {"total changes", {header(10, 20, true), other_total}},
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

TEST(MleAssembler, WorkerHeaderCheck) {
  std::string error;
  EXPECT_TRUE(check_worker_header(header(10, 10, false), 1024, &error));
  MessageHeader h = header(10, 10, false);
  h.op = 0;
  EXPECT_FALSE(check_worker_header(h, 1024, &error));
  h = header(10, 10, false, wire::kMaxOp + 1);
  EXPECT_FALSE(check_worker_header(h, 1024, &error));
  h = header(10, 10, false);
  h.status = uint32_t(wire::Status::kDied);  // host-only status
  EXPECT_FALSE(check_worker_header(h, 1024, &error));
  h = header(10, 10, false);
  h.flags = 2;
  EXPECT_FALSE(check_worker_header(h, 1024, &error));
  EXPECT_FALSE(check_worker_header(header(1025, 1025, false), 1024, &error));
  EXPECT_FALSE(check_worker_header(header(10, wire::kMaxMessageSize + 1, true),
                                   1024, &error));
}

}  // namespace mle_codec_unittest
