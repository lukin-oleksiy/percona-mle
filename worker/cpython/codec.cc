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
  @file worker/cpython/codec.cc

  Decoding of values, rows and columns for the CPython worker: values as Python objects, rows as
  row tuples, and columns of fetch_columns straight into typed buffers.
*/

#include "codec.hpp"

#include <datetime.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace mle::cpython {

using wire::Tag;

Helpers &helpers() {
  static Helpers h;
  return h;
}

namespace {

constexpr size_t kTemporalSize = 15;  // u16 year, u8 month, u8 day, u32 hour, u8 minute, u8 second, u32 us, u8 neg

Ref unicode(std::string_view s) {
  return Ref::steal(PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "strict"));
}

Ref call(PyObject *f, PyObject *a) { return Ref::steal(PyObject_CallOneArg(f, a)); }

Ref temporal(Tag tag, Reader &r) {
  const auto year = r.get<uint16_t>();
  const auto month = r.get<uint8_t>();
  const auto day = r.get<uint8_t>();
  const auto hour = r.get<uint32_t>();
  const auto minute = r.get<uint8_t>();
  const auto second = r.get<uint8_t>();
  const auto micro = r.get<uint32_t>();
  const bool neg = r.get<uint8_t>() != 0;
  PyObject *v = nullptr;
  if (tag == Tag::kTime) {
    const long secs = static_cast<long>(hour) * 3600 + minute * 60 + second;
    if (hour <= wire::kMaxTimeHour && micro < 1000000)
      v = PyDelta_FromDSU(0, static_cast<int>(neg ? -secs : secs), static_cast<int>(neg ? -long(micro) : long(micro)));
  } else if (tag == Tag::kDate) {
    v = PyDate_FromDate(year, month, day);
  } else if (hour < 24 && micro < 1000000) {
    v = PyDateTime_FromDateAndTime(year, month, day, static_cast<int>(hour), minute, second, static_cast<int>(micro));
  }
  if (v != nullptr) return Ref::steal(v);
  // Out of Python's range (zero dates, a corrupt value): the Python helper raises the right error.
  PyErr_Clear();
  return Ref::steal(PyObject_CallFunction(helpers().temporal.get(), "iIIIIIIIi", static_cast<int>(tag), year, month,
                                          day, hour, minute, second, micro, neg ? 1 : 0));
}

Ref decode_tagged(Tag tag, Reader &r, PyObject *body, PyObject *vector) {
  switch (tag) {
    case Tag::kNull:
      return Ref::borrow(Py_None);
    case Tag::kI64:
      return Ref::steal(PyLong_FromLongLong(r.get<int64_t>()));
    case Tag::kU64:
      return Ref::steal(PyLong_FromUnsignedLongLong(r.get<uint64_t>()));
    case Tag::kF64:
      return Ref::steal(PyFloat_FromDouble(r.get<double>()));
    case Tag::kF32:
      return Ref::steal(PyFloat_FromDouble(r.get<float>()));
    case Tag::kString:
      return unicode(r.str());
    case Tag::kBytes: {
      const auto n = r.get<uint32_t>();
      auto s = r.take(n);
      return Ref::steal(PyBytes_FromStringAndSize(reinterpret_cast<const char *>(s.data()), Py_ssize_t(s.size())));
    }
    case Tag::kDecimal:
      return call(helpers().decimal.get(), unicode(r.str()).get());
    case Tag::kJson:
      return call(helpers().json_loads.get(), unicode(r.str()).get());
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
    case Tag::kTimestamp:
      return temporal(tag, r);
    case Tag::kVector: {
      const auto dims = r.get<uint32_t>();
      if (dims > wire::kMaxVectorDimensions) throw Malformed{"VECTOR too large"};
      r.align();
      const size_t pos = r.pos();
      r.take(size_t{dims} * 4);
      return vector != nullptr ? Ref::steal(PyObject_CallFunction(vector, "In", dims, Py_ssize_t(pos)))
                               : Ref::steal(PyObject_CallFunction(helpers().new_vector.get(), "OnI", body,
                                                                  Py_ssize_t(pos), dims));
    }
  }
  throw Malformed{"unknown value tag"};
}

}  // namespace

Ref decode_value(Reader &r, PyObject *body, PyObject *vector) {
  return decode_tagged(static_cast<Tag>(r.get<uint8_t>()), r, body, vector);
}

void skip_value(Reader &r) {
  switch (static_cast<Tag>(r.get<uint8_t>())) {
    case Tag::kNull:
      return;
    case Tag::kI64:
    case Tag::kU64:
    case Tag::kF64:
      r.take(8);
      return;
    case Tag::kF32:
      r.take(4);
      return;
    case Tag::kDecimal:
    case Tag::kString:
    case Tag::kBytes:
    case Tag::kJson:
      r.take(r.get<uint32_t>());
      return;
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
    case Tag::kTimestamp:
      r.take(kTemporalSize);
      return;
    case Tag::kVector: {
      const auto dims = r.get<uint32_t>();
      r.align();
      r.take(size_t{dims} * 4);
      return;
    }
  }
  throw Malformed{"unknown value tag"};
}

// ------------------------------------------------------------------------------ module functions

/** setup(decimal, json_loads, temporal, new_vector, demote) */
Ref py_setup(PyObject *args) {
  PyObject *d, *j, *t, *v, *m;
  if (!PyArg_ParseTuple(args, "OOOOO", &d, &j, &t, &v, &m)) throw PyErrorSet{};
  PyDateTime_IMPORT;
  if (PyDateTimeAPI == nullptr) throw PyErrorSet{};
  Helpers &h = helpers();
  h.decimal = Ref::borrow(d);
  h.json_loads = Ref::borrow(j);
  h.temporal = Ref::borrow(t);
  h.new_vector = Ref::borrow(v);
  h.demote = Ref::borrow(m);
  return Ref::borrow(Py_None);
}

/** decode(body, pos, n, vector=None) -> (list of n values, pos) */
Ref py_decode(PyObject *args) {
  PyObject *body, *vector = Py_None;
  Py_ssize_t pos, n;
  if (!PyArg_ParseTuple(args, "Onn|O", &body, &pos, &n, &vector)) throw PyErrorSet{};
  Buffer buf(body, PyBUF_SIMPLE);
  Reader r(buf.bytes(), size_t(pos));
  if (n < 0 || size_t(n) > wire::kMaxValues) throw Malformed{"bad value count"};
  Ref list = Ref::steal(PyList_New(n));
  for (Py_ssize_t i = 0; i < n; i++)
    PyList_SET_ITEM(list.get(), i, decode_value(r, body, vector == Py_None ? nullptr : vector).release());
  return Ref::steal(Py_BuildValue("(Nn)", list.release(), Py_ssize_t(r.pos())));
}

/** decode_rows(body, pos, nrows, ncols, row_type) -> (list of rows, pos). row_type: a tuple subclass
    (filled in place, as tuple's own subtype constructor does) or None for plain tuples. */
Ref py_decode_rows(PyObject *args) {
  PyObject *body, *row_type;
  Py_ssize_t pos, nrows, ncols;
  if (!PyArg_ParseTuple(args, "OnnnO", &body, &pos, &nrows, &ncols, &row_type)) throw PyErrorSet{};
  if (row_type != Py_None &&
      (!PyType_Check(row_type) || !PyType_IsSubtype(reinterpret_cast<PyTypeObject *>(row_type), &PyTuple_Type))) {
    PyErr_SetString(PyExc_TypeError, "row_type must be a tuple subclass");
    throw PyErrorSet{};
  }
  if (nrows < 0 || ncols < 0) throw Malformed{"bad row count"};
  Buffer buf(body, PyBUF_SIMPLE);
  Reader r(buf.bytes(), size_t(pos));
  auto *type = reinterpret_cast<PyTypeObject *>(row_type);
  Ref list = Ref::steal(PyList_New(nrows));
  for (Py_ssize_t i = 0; i < nrows; i++) {
    Ref row = Ref::steal(row_type == Py_None ? PyTuple_New(ncols) : type->tp_alloc(type, ncols));
    for (Py_ssize_t c = 0; c < ncols; c++) PyTuple_SET_ITEM(row.get(), c, decode_value(r, body, nullptr).release());
    PyList_SET_ITEM(list.get(), i, row.release());
  }
  return Ref::steal(Py_BuildValue("(Nn)", list.release(), Py_ssize_t(r.pos())));
}

namespace {

/** A column of fetch_columns while it is filled (the Python Collector's attributes). */
enum class Mode : long { kSkip = 0, kVector = 1, kInt64 = 2, kFloat64 = 3, kList = 4 };

struct Column {
  PyObject *obj = nullptr;  // the Collector (borrowed from the caller's list)
  Mode mode = Mode::kSkip;
  Ref data;                 // bytearray (typed modes) or list
  uint32_t dims = 0;        // VECTOR dimensions (0: not known yet)
  Py_ssize_t count = 0;     // values in data (typed modes)
  Py_ssize_t expect = 0;    // rows of the whole result set (0: unknown), for the first allocation

  long attr_long(const char *name) {
    Ref v = Ref::steal(PyObject_GetAttrString(obj, name));
    const long x = PyLong_AsLong(v.get());
    if (x == -1 && PyErr_Occurred()) throw PyErrorSet{};
    return x;
  }
  void load() {
    mode = static_cast<Mode>(attr_long("mode"));
    data = Ref::steal(PyObject_GetAttrString(obj, "data"));
    dims = static_cast<uint32_t>(attr_long("dims"));
    count = attr_long("count");
    expect = attr_long("expect");
  }
  void store() {
    Ref d = Ref::steal(PyLong_FromUnsignedLong(dims));
    Ref c = Ref::steal(PyLong_FromSsize_t(count));
    if (PyObject_SetAttrString(obj, "dims", d.get()) != 0 || PyObject_SetAttrString(obj, "count", c.get()) != 0)
      throw PyErrorSet{};
  }
  /** Room for one more item of item_size bytes; returns where it goes. */
  std::byte *slot(size_t item_size) {
    const size_t need = (size_t(count) + 1) * item_size;
    const auto have = size_t(PyByteArray_GET_SIZE(data.get()));
    if (need > have) {
      const size_t want = std::max({need, have * 2, size_t(std::max<Py_ssize_t>(expect, 0)) * item_size});
      if (PyByteArray_Resize(data.get(), Py_ssize_t(want)) != 0) throw PyErrorSet{};
    }
    return reinterpret_cast<std::byte *>(PyByteArray_AS_STRING(data.get())) + size_t(count) * item_size;
  }
  /** The typed column becomes a list (Python: demote()); its values so far are converted. */
  void demote() {
    store();
    Ref r = Ref::steal(PyObject_CallOneArg(helpers().demote.get(), obj));
    load();
    if (mode != Mode::kList) throw Malformed{"demote() left a typed column"};
  }
};

bool add_typed(Column &c, Tag tag, Reader &r) {
  switch (c.mode) {
    case Mode::kVector: {
      if (tag != Tag::kVector) return false;
      const auto dims = r.get<uint32_t>();
      if (dims == 0 || dims > wire::kMaxVectorDimensions || (c.dims != 0 && dims != c.dims)) return false;
      r.align();
      c.dims = dims;
      auto src = r.take(size_t{dims} * 4);
      std::memcpy(c.slot(src.size()), src.data(), src.size());
      c.count++;
      return true;
    }
    case Mode::kInt64: {
      int64_t v;
      if (tag == Tag::kI64) {
        v = r.get<int64_t>();
      } else if (tag == Tag::kU64) {
        const auto u = r.get<uint64_t>();
        if (u > uint64_t(std::numeric_limits<int64_t>::max())) return false;
        v = int64_t(u);
      } else {
        return false;
      }
      std::memcpy(c.slot(8), &v, 8);
      c.count++;
      return true;
    }
    case Mode::kFloat64: {
      double v;
      if (tag == Tag::kF64) v = r.get<double>();
      else if (tag == Tag::kF32) v = r.get<float>();
      else return false;
      std::memcpy(c.slot(8), &v, 8);
      c.count++;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

/** decode_columns(body, pos, nrows, collectors) -> pos. collectors: one Collector (or None: the column
    is not wanted) per column; values go straight into their typed buffers (VECTOR: float32 rows of a
    matrix; integers: int64; floating point: float64); a value that does not fit makes the column a
    list (Collector.demote). */
Ref py_decode_columns(PyObject *args) {
  PyObject *body, *collectors;
  Py_ssize_t pos, nrows;
  if (!PyArg_ParseTuple(args, "OnnO!", &body, &pos, &nrows, &PyList_Type, &collectors)) throw PyErrorSet{};
  if (nrows < 0) throw Malformed{"bad row count"};
  Buffer buf(body, PyBUF_SIMPLE);
  Reader r(buf.bytes(), size_t(pos));
  const Py_ssize_t ncols = PyList_GET_SIZE(collectors);
  std::vector<Column> cols(static_cast<size_t>(ncols));
  for (Py_ssize_t c = 0; c < ncols; c++) {
    cols[size_t(c)].obj = PyList_GET_ITEM(collectors, c);
    if (cols[size_t(c)].obj != Py_None) cols[size_t(c)].load();
  }
  for (Py_ssize_t i = 0; i < nrows; i++) {
    for (Column &c : cols) {
      if (c.obj == Py_None || c.mode == Mode::kSkip) {
        skip_value(r);
        continue;
      }
      const Reader start = r;
      const auto tag = static_cast<Tag>(r.get<uint8_t>());
      if (c.mode != Mode::kList && add_typed(c, tag, r)) continue;
      r = start;
      if (c.mode != Mode::kList) c.demote();
      Ref v = decode_value(r, body, nullptr);
      if (PyList_Append(c.data.get(), v.get()) != 0) throw PyErrorSet{};
    }
  }
  for (Column &c : cols)
    if (c.obj != Py_None && c.mode != Mode::kList && c.mode != Mode::kSkip) c.store();
  return Ref::steal(PyLong_FromSsize_t(Py_ssize_t(r.pos())));
}

}  // namespace mle::cpython
