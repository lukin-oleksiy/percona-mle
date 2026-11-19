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

// The CPython worker's codec (worker/cpython/codec.cc, exp design note 62):
// Reader and Writer bounds; every value tag of messages the host encodes
// (mle/codec.h) decoded into the Python values routines get; rows into row
// tuples and columns into typed buffers, with the fallback to lists; and
// malformed input (truncated, random) gives Python exceptions, never a crash.
//
// The interpreter is the one the worker embeds, with the worker's code and
// site-packages (numpy) on sys.path. MLE_FUZZ_ITERATIONS (default 20000) and
// MLE_FUZZ_SEED control the random part.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "codec.hpp"
#include "mle/codec.h"
#include "mle/protocol.h"
#include "module.hpp"

namespace mle_cpython_codec_unittest {

using mle::Value;
using mle::cpython::Malformed;
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
_mlewire.setup(decimal.Decimal, json.loads, wire.temporal, wire.new_vector, wire.demote, lambda *a: None, float)
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

TEST(CpythonCodec, ReaderBounds) {
  const std::array<std::byte, 6> b{std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{'a'},
                                   std::byte{'b'}};
  Reader r(b, 0);
  EXPECT_THROW(r.str(), Malformed);  // length 4, 2 bytes left
  Reader r2(b, 4);
  EXPECT_EQ(r2.get<uint16_t>(), uint16_t('a' | ('b' << 8)));
  EXPECT_THROW(r2.get<uint8_t>(), Malformed);
  EXPECT_THROW(Reader(b, 7), Malformed);
  Reader r3(b, 3);
  EXPECT_THROW(r3.align(), Malformed);  // to 16: beyond the end
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
                               Value::temporal(Tag::kTimestamp, datetime), Value::temporal(Tag::kTime, time),
                               Value::vector({1.0f, -2.5f, 3.0f})}));
  EXPECT_TRUE(py(R"(
v, end = _mlewire.decode(body, 16, 14)
assert end == len(body), (end, len(body))
assert v[:9] == [None, -5, 2**64 - 1, 1.25, 0.5, decimal.Decimal('-12.345'), 'héllo', b'A\xff\x00', {'a': [1, 2]}], v
assert type(v[5]) is decimal.Decimal
assert v[9] == dt.date(2026, 11, 19) and type(v[9]) is dt.date
assert v[10] == v[11] == dt.datetime(2026, 11, 19, 12, 34, 56, 123456)
assert v[12] == -dt.timedelta(hours=838, minutes=59, seconds=59)
assert v[13].dtype == np.float32 and v[13].tolist() == [1.0, -2.5, 3.0]
seen = []
v, _ = _mlewire.decode(body, 16, 14, lambda dims, pos: seen.append((dims, pos % 16)) or 'V')
assert v[13] == 'V' and seen == [(3, 0)], seen
)"));
}

TEST(CpythonCodec, ZeroDate) {
  set_bytes("body", call_body({Value::temporal(Tag::kDatetime, mle::Temporal{})}));
  EXPECT_TRUE(py(R"(
try:
    _mlewire.decode(body, 16, 1)
    raise AssertionError('no error')
except wire.ConversionError as e:
    assert 'zero date' in str(e), e
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
  EXPECT_TRUE(py(R"(
for n in range(pos, len(body)):
    for f in (lambda b: _mlewire.decode_rows(b, pos, 3, 3, None),
              lambda b: _mlewire.decode_columns(b, pos, 3, [wire.Collector(wire.MODE_LIST, 0),
                                                           wire.Collector(wire.MODE_VECTOR, 0), None])):
        try:
            f(body[:n])
            raise AssertionError('truncated at %d accepted' % n)
        except RuntimeError as e:
            assert 'short' in str(e) or 'beyond' in str(e), e
)"));
}

TEST(CpythonCodec, RandomInput) {
  const char *it = std::getenv("MLE_FUZZ_ITERATIONS");
  const char *seed = std::getenv("MLE_FUZZ_SEED");
  const std::string setup = std::string("iterations = ") + (it ? it : "20000") + "\nseed = " + (seed ? seed : "20261119");
  ASSERT_TRUE(py(setup.c_str()));
  EXPECT_TRUE(py(R"(
rnd = random.Random(seed)
tags = bytes(range(16))
for i in range(iterations):
    n = rnd.randrange(0, 64)
    # Mostly valid tags, so that decoding gets past the first byte.
    b = bytes(rnd.choice(tags) if rnd.random() < 0.3 else rnd.randrange(256) for _ in range(n))
    try:
        if i % 3 == 0:
            _mlewire.decode(b, 0, rnd.randrange(0, 4))
        elif i % 3 == 1:
            _mlewire.decode_rows(b, 0, rnd.randrange(0, 3), rnd.randrange(0, 3), None)
        else:
            _mlewire.decode_columns(b, 0, rnd.randrange(0, 3),
                                    [wire.Collector(rnd.randrange(0, 5), 0) for _ in range(rnd.randrange(0, 3))])
    except (RuntimeError, ValueError, UnicodeDecodeError, OverflowError, decimal.InvalidOperation,
            json.JSONDecodeError, wire.ConversionError):
        pass
    except Exception as e:
        raise AssertionError('iteration %d (seed %d): %r on %r' % (i, seed, e, b))
)"));
}

}  // namespace mle_cpython_codec_unittest

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new mle_cpython_codec_unittest::PythonEnvironment);
  return RUN_ALL_TESTS();
}
