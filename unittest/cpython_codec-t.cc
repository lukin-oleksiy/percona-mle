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

// The CPython worker's codec (worker/cpython/codec.cc, docs/protocol.md §7):
// Reader and Writer bounds; every value tag of messages the host encodes
// (mle/codec.h) decoded into the Python values routines get, ill-formed text
// repaired; rows into row tuples and columns into typed buffers, with the
// fallback to lists; what the worker writes (text repaired with U+FFFD, the
// domains of §7.2); and a message that breaks the protocol (truncated, an
// unknown tag) ends the process with a protocol failure (§1), never a crash.
//
// The interpreter is the one the worker embeds, with the worker's code and
// site-packages (numpy) on sys.path.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "codec.hpp"
#include "mle/codec.h"
#include "mle/protocol.h"
#include "module.hpp"

namespace mle_cpython_codec_unittest {

using mle::Value;
using mle::cpython::Reader;
using mle::cpython::Writer;
using mle::wire::Tag;

PyObject *g_globals = nullptr;

class PythonEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    ASSERT_EQ(PyImport_AppendInittab("_mlewire", mle::cpython::init_mlewire), 0);
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.write_bytecode = 0;
    config.module_search_paths_set = 1;
    for (const char *p : {MLE_PYTHON_STDLIB, MLE_PYTHON_DYNLOAD, MLE_CPYTHON_CODE, MLE_CPYTHON_SITE}) {
      std::wstring w(p, p + std::char_traits<char>::length(p));
      ASSERT_FALSE(PyStatus_Exception(PyWideStringList_Append(&config.module_search_paths, w.c_str())));
    }
    const PyStatus st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    ASSERT_FALSE(PyStatus_Exception(st)) << st.err_msg;
    g_globals = PyModule_GetDict(PyImport_AddModule("__main__"));
    ASSERT_TRUE(run(R"(
import _mlewire, decimal, json, random, numpy as np, datetime as dt
from mle_cpython import wire
_mlewire.setup(decimal.Decimal, json.loads, wire.temporal, wire.new_vector, wire.demote, lambda *a: None, float,
               wire.timestamp)
ARGS = 20  # a CALL's arguments follow routine_id, library_epoch and their count
)"));
  }

  /** Runs Python statements in __main__; false (and the traceback) if they raise. */
  static bool run(const char *code) {
    PyObject *r = PyRun_String(code, Py_file_input, g_globals, g_globals);
    if (r == nullptr) {
      PyErr_Print();
      return false;
    }
    Py_DECREF(r);
    return true;
  }
};

bool py(const char *code) { return PythonEnvironment::run(code); }

void set_bytes(const char *name, const std::string &b) {
  PyObject *o = PyBytes_FromStringAndSize(b.data(), Py_ssize_t(b.size()));
  ASSERT_NE(o, nullptr);
  ASSERT_EQ(PyDict_SetItemString(g_globals, name, o), 0);
  Py_DECREF(o);
}

std::string call_body(std::vector<Value> args) {
  mle::CallMsg m;
  m.routine_id = 7;
  m.args = std::move(args);
  std::string out;
  mle::encode(m, &out);
  return out;
}

mle::Column column(const char *name, Tag tag) {
  mle::ParamType t;
  t.tag = tag;
  return {name, "t", t};
}

/** A kRows SQL_RESULT and the position of its first row. */
std::pair<std::string, size_t> rows_body(std::vector<mle::Column> cols, std::vector<Value> rows, uint64_t total) {
  mle::SqlResultMsg m;
  m.kind = mle::wire::SqlResultKind::kRows;
  m.columns = std::move(cols);
  m.total_rows = total;
  std::string empty;
  mle::encode(m, &empty);
  m.rows = std::move(rows);
  std::string out;
  mle::encode(m, &out);
  // The trailer after the rows: u8 more, u8 more results, u32 warnings, u64 microseconds.
  return {out, empty.size() - 14};
}

/** The statement ends the process as a protocol failure does (common::protocol_failure). */
#define EXPECT_PROTOCOL_FAILURE(statement) EXPECT_EXIT(statement, ::testing::ExitedWithCode(1), "protocol failure")

TEST(CpythonCodec, ReaderBounds) {
  const std::array<std::byte, 6> b{std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{'a'},
                                   std::byte{'b'}};
  EXPECT_PROTOCOL_FAILURE(Reader(b, 0).str());  // length 4, 2 bytes left
  Reader r2(b, 4);
  EXPECT_EQ(r2.get<uint16_t>(), uint16_t('a' | ('b' << 8)));
  EXPECT_PROTOCOL_FAILURE(r2.get<uint8_t>());
  EXPECT_PROTOCOL_FAILURE(Reader(b, 7));
  EXPECT_PROTOCOL_FAILURE(Reader(b, 3).align());  // to 16: beyond the end
  EXPECT_PROTOCOL_FAILURE(Reader(b, 0).count(100, 2));  // 4 elements of 2 bytes in 2 bytes
  // A bool is not checked for 0 or 1, nor a VECTOR's padding for zeros (§1).
  Reader r3(b, 0);
  EXPECT_TRUE(r3.boolean());
}

TEST(CpythonCodec, WriterFull) {
  std::array<std::byte, 6> b{};
  Writer w(b);
  EXPECT_TRUE(w.put(uint32_t{1}));
  EXPECT_FALSE(w.put(uint32_t{2}));  // 2 bytes left: nothing written
  EXPECT_EQ(w.size(), 4u);
  EXPECT_FALSE(w.str(""));           // its u32 length does not fit either
}

TEST(CpythonCodec, EveryTag) {
  mle::Temporal date{2026, 11, 19};
  mle::Temporal datetime{2026, 11, 19, 12, 34, 56, 123456};
  mle::Temporal time{};
  time.hour = 838;
  time.minute = 59;
  time.second = 59;
  time.negative = true;
  set_bytes("body", call_body({Value::null(), Value::i64(-5), Value::u64(18446744073709551615ULL), Value::f64(1.25),
                               Value::f32(0.5f), Value::decimal("-12.345"), Value::string("h\xC3\xA9llo"),
                               Value::bytes(std::string("A\xFF\0", 3)), Value::json(R"({"a": [1, 2]})"),
                               Value::temporal(Tag::kDate, date), Value::temporal(Tag::kDatetime, datetime),
                               Value::timestamp(1704067200000000), Value::temporal(Tag::kTime, time),
                               Value::vector({1.0f, -2.5f, 3.0f})}));
  EXPECT_TRUE(py(R"(
v, end = _mlewire.decode(body, ARGS, 14)
assert end == len(body), (end, len(body))
assert v[:9] == [None, -5, 2**64 - 1, 1.25, 0.5, decimal.Decimal('-12.345'), 'héllo', b'A\xff\x00', {'a': [1, 2]}], v
assert type(v[5]) is decimal.Decimal
assert v[9] == dt.date(2026, 11, 19) and type(v[9]) is dt.date
assert v[10] == dt.datetime(2026, 11, 19, 12, 34, 56, 123456)
# A TIMESTAMP is an instant: an aware datetime in the zone of OPTIONS (B.4), +00:00 here.
assert v[11] == dt.datetime(2024, 1, 1, tzinfo=dt.timezone.utc) and v[11].tzinfo is wire.ZONE, repr(v[11])
assert str(v[11]) == '2024-01-01 00:00:00+00:00', str(v[11])
wire.ZONE = dt.timezone(dt.timedelta(hours=5, minutes=30))
ts = _mlewire.decode(body, ARGS, 14)[0][11]
assert ts.replace(tzinfo=None) == dt.datetime(2024, 1, 1, 5, 30) and ts.utcoffset() == dt.timedelta(hours=5, minutes=30), ts
import zoneinfo
wire.ZONE = zoneinfo.ZoneInfo('Europe/Vilnius')
ts = _mlewire.decode(body, ARGS, 14)[0][11]
assert repr(ts) == 'datetime.datetime(2024, 1, 1, 2, 0, tzinfo=zoneinfo.ZoneInfo(key=\'Europe/Vilnius\'))', repr(ts)
wire.ZONE = dt.timezone(dt.timedelta(0))
assert v[12] == -dt.timedelta(hours=838, minutes=59, seconds=59)
assert v[13].dtype == np.float32 and v[13].tolist() == [1.0, -2.5, 3.0]
seen = []
v, _ = _mlewire.decode(body, ARGS, 14, lambda dims, pos: seen.append((dims, pos % 16)) or 'V')
assert v[13] == 'V' and seen == [(3, 0)], seen
)"));
}

TEST(CpythonCodec, ZeroDate) {
  // Appendix B.4: a date Python cannot hold and the zero timestamp are None, as routine
  // arguments and in rows (MVP review D1).
  set_bytes("body", call_body({Value::temporal(Tag::kDatetime, mle::Temporal{})}));
  set_bytes("body2", call_body({Value::timestamp(mle::wire::kZeroTimestamp)}));
  EXPECT_TRUE(py(R"(
for b in (body, body2):
    v, _ = _mlewire.decode(b, ARGS, 1)
    assert v == [None], v
assert wire.timestamp(wire.ZERO_TIMESTAMP, 1) is None
)"));
}

TEST(CpythonCodec, HostTextAndFloats) {
  // Ill-formed UTF-8 in a STRING is repaired, each maximal subpart by U+FFFD (§7.1); any float
  // bit pattern is accepted (§7.2); a VECTOR's padding is not looked at.
  std::string body = call_body({Value::string(std::string("a\xF0\x80\x80z\xFF", 6)), Value::f64(0.0), Value::f32(0.0f),
                                Value::vector({1.5f})});
  const size_t f64_at = 20 + 1 + 4 + 6 + 1;
  std::memset(body.data() + f64_at, 0xFF, 8);      // a NaN
  std::memset(body.data() + f64_at + 9, 0xFF, 4);  // a NaN
  body[f64_at + 8 + 5 + 5 + 1] = 1;                // inside the padding
  set_bytes("body", body);
  EXPECT_TRUE(py(R"(
import math
v, _ = _mlewire.decode(body, ARGS, 4)
assert v[0] == 'a\ufffd\ufffd\ufffdz\ufffd', ascii(v[0])
assert math.isnan(v[1]) and math.isnan(v[2]), v
assert v[3].tolist() == [1.5], v[3]
)"));
}

TEST(CpythonCodec, WorkerValues) {
  // What the worker sends: text repaired with U+FFFD, never '?'; the domains of §7.2.
  EXPECT_TRUE(py(R"(
assert wire.utf8('a\ud800b\udfff') == 'a\ufffdb\ufffd'.encode()
assert wire.cut('h\u00e9llo'.encode(), 2) == b'h'
w = wire.Writer()
w.param('x\udc00')
assert bytes(w.b) == b'\x06\x04\x00\x00\x00x\xef\xbf\xbd', bytes(w.b)
b = bytearray()
from mle_cpython import sql
assert _mlewire.encode_values(b, ['\ud83d', 1, None, 2.5, b'\xff'], sql._encode_one) == 5
assert bytes(b[:8]) == b'\x06\x03\x00\x00\x00\xef\xbf\xbd', bytes(b)
t = wire.ParamType(wire.T_TIMESTAMP, False, 0, 0, 0, 'TIMESTAMP')
w = wire.Writer()
w.write_as(dt.datetime(2024, 1, 1, tzinfo=dt.timezone.utc), t)   # an instant: TIMESTAMP
w.write_as(dt.datetime(2024, 1, 1), t)                             # naive: DATETIME, the host converts
assert bytes(w.b) == bytes.fromhex('0b00202110d70d0600' '0ae80701010000000000000000000000'), w.b.hex()
w = wire.Writer()
w.write_as(dt.datetime(2024, 1, 1, 1, tzinfo=dt.timezone(dt.timedelta(hours=1))),
           wire.ParamType(wire.T_DATETIME, False, 0, 0, 0, 'DATETIME'))  # aware as DATETIME: TIMESTAMP
assert bytes(w.b) == bytes.fromhex('0b00202110d70d0600'), w.b.hex()
for bad, t in ((dt.datetime(1969, 1, 1, tzinfo=dt.timezone.utc), t),
               (float('nan'), wire.ParamType(wire.T_F64, False, 0, 0, 0, 'DOUBLE')),
               (1e300, wire.ParamType(wire.T_F32, False, 0, 0, 0, 'FLOAT')),
               ([], wire.ParamType(wire.T_VECTOR, False, 0, 0, 0, 'VECTOR')),
               ([0.0] * 16384, wire.ParamType(wire.T_VECTOR, False, 0, 0, 0, 'VECTOR'))):
    try:
        wire.Writer().write_as(bad, t)
        raise AssertionError('no error for %r' % (bad,))
    except wire.ConversionError:
        pass
w = wire.Writer()
w.param(dt.datetime(2024, 1, 1, tzinfo=dt.timezone.utc))   # aware: TIMESTAMP
w.param(dt.datetime(2024, 1, 1))                           # naive: DATETIME
assert w.b[0] == wire.T_TIMESTAMP and w.b[9] == wire.T_DATETIME, w.b.hex()
)"));
}

TEST(CpythonCodec, RowsAndColumns) {
  std::vector<Value> rows;
  for (int i = 0; i < 5; i++) {
    rows.push_back(Value::i64(i));
    rows.push_back(Value::vector({float(i), float(i) + 0.5f}));
    rows.push_back(i == 2 ? Value::null() : Value::string("r" + std::to_string(i)));
    rows.push_back(i == 3 ? Value::null() : Value::f64(i * 1.5));
    rows.push_back(i == 4 ? Value::u64(1ULL << 63) : Value::u64(i));
  }
  auto [body, pos] = rows_body({column("id", Tag::kI64), column("v", Tag::kVector), column("s", Tag::kString),
                                column("x", Tag::kF64), column("u", Tag::kU64)},
                               rows, 5);
  set_bytes("body", body);
  PyDict_SetItemString(g_globals, "pos", PyLong_FromSize_t(pos));
  EXPECT_TRUE(py(R"(
Row = type('Row', (tuple,), {'__slots__': ()})
rs, end = _mlewire.decode_rows(body, pos, 5, 5, Row)
assert end == len(body) - 14, (end, len(body))
assert type(rs[0]) is Row and rs[1][0] == 1 and rs[2][2] is None and rs[4][4] == 2**63, rs
assert rs[3][1].tolist() == [3.0, 3.5]
plain, _ = _mlewire.decode_rows(body, pos, 5, 5, None)
assert type(plain[0]) is tuple and plain[0][0] == 0

c = [wire.Collector(m, 5) for m in (wire.MODE_INT64, wire.MODE_VECTOR, wire.MODE_LIST, wire.MODE_FLOAT64,
                                    wire.MODE_INT64)]
end2 = _mlewire.decode_columns(body, pos, 5, c)
assert end2 == end
ids, m, s, x, u = (k.result() for k in c)
assert ids.dtype == np.int64 and ids.tolist() == [0, 1, 2, 3, 4]
assert m.dtype == np.float32 and m.shape == (5, 2) and m[4].tolist() == [4.0, 4.5]
assert s == ['r0', 'r1', None, 'r3', 'r4']
assert x == [0.0, 1.5, 3.0, None, 6.0], x               # a NULL: a list
assert u == [0, 1, 2, 3, 2**63], u                      # beyond int64: a list
skipped = _mlewire.decode_columns(body, pos, 5, [None] * 5)
assert skipped == end
# Two chunks into the same collectors.
c2 = wire.Collector(wire.MODE_VECTOR, 0)
_mlewire.decode_columns(body, pos, 5, [None, c2, None, None, None])
_mlewire.decode_columns(body, pos, 5, [None, c2, None, None, None])
assert c2.result().shape == (10, 2)
)"));
}

TEST(CpythonCodec, VectorColumnDemoted) {
  std::vector<Value> rows = {Value::vector({1, 2}), Value::null(), Value::vector({3, 4, 5})};
  auto [body, pos] = rows_body({column("v", Tag::kVector)}, rows, 3);
  set_bytes("body", body);
  PyDict_SetItemString(g_globals, "pos", PyLong_FromSize_t(pos));
  EXPECT_TRUE(py(R"(
c = wire.Collector(wire.MODE_VECTOR, 3)
_mlewire.decode_columns(body, pos, 3, [c])
r = c.result()
assert type(r) is list and r[1] is None and r[0].tolist() == [1, 2] and r[2].tolist() == [3, 4, 5], r
)"));
}

TEST(CpythonCodec, Truncated) {
  std::vector<Value> rows;
  for (int i = 0; i < 3; i++) {
    rows.push_back(Value::string("abc"));
    rows.push_back(Value::vector({1, 2, 3}));
    rows.push_back(Value::temporal(Tag::kDate, mle::Temporal{2026, 1, 2}));
  }
  auto [body, pos] = rows_body({column("s", Tag::kString), column("v", Tag::kVector), column("d", Tag::kDate)},
                               rows, 3);
  set_bytes("body", body.substr(0, body.size() - 14));
  PyDict_SetItemString(g_globals, "pos", PyLong_FromSize_t(pos));
  // Cut anywhere, the rows end the process and are not read beyond the message.
  for (const size_t n : {pos, pos + 1, pos + 6, pos + 20, body.size() - 30, body.size() - 15}) {
    PyDict_SetItemString(g_globals, "n", PyLong_FromSize_t(n));
    EXPECT_PROTOCOL_FAILURE(py("_mlewire.decode_rows(body[:n], pos, 3, 3, None)"));
    EXPECT_PROTOCOL_FAILURE(py("_mlewire.decode_columns(body[:n], pos, 3, [wire.Collector(wire.MODE_LIST, 0), "
                               "wire.Collector(wire.MODE_VECTOR, 0), None])"));
  }
  // An unknown tag, and counts beyond the bytes there are.
  EXPECT_PROTOCOL_FAILURE(py("_mlewire.decode(bytes([14]), 0, 1)"));
  EXPECT_PROTOCOL_FAILURE(py("_mlewire.decode(bytes(4), 0, 5)"));
  EXPECT_PROTOCOL_FAILURE(py("_mlewire.decode_rows(bytes(4), 0, 3, 2, None)"));
  EXPECT_PROTOCOL_FAILURE(py("wire.Reader(bytes(3)).u32()"));
  EXPECT_PROTOCOL_FAILURE(py("wire.Reader(bytes([9, 0, 0, 0, 65])).str()"));
  EXPECT_PROTOCOL_FAILURE(py("wire.Reader(bytes([14]) + bytes(17)).type()"));
  EXPECT_TRUE(py("assert wire.Reader(bytes([13]) + bytes(17)).type().tag == 13"));
}

}  // namespace mle_cpython_codec_unittest

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new mle_cpython_codec_unittest::PythonEnvironment);
  return RUN_ALL_TESTS();
}
