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
  row tuples, and columns of fetch_columns straight into typed buffers. A value is decoded by its
  own tag (§1 item 2); lengths are checked against the message, domains are not (§1).
*/

#include "codec.hpp"

#include <datetime.h>

#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace mle::cpython {

using wire::Tag;

Helpers &helpers() {
  static Helpers h;
  return h;
}

ValueRules &value_rules() {
  static ValueRules r = ValueRules::kArguments;
  return r;
}

Ref host_text(std::string_view s) {
  return Ref::steal(PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "replace"));
}

std::string raised_text() {
  std::string text;
  if (PyObject *e = PyErr_GetRaisedException(); e != nullptr) {
    if (PyObject *shown = PyObject_ASCII(e); shown != nullptr) {
      if (const char *s = PyUnicode_AsUTF8(shown); s != nullptr) text = s;
      Py_DECREF(shown);
    }
    PyErr_Clear();
    Py_DECREF(e);
  }
  return text;
}

namespace {

constexpr size_t kCivilSize = 15;  // u16 year, u8 month, u8 day, u32 hour, u8 minute, u8 second, u32 us, bool neg

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
  PyObject *f = value_rules() == ValueRules::kArguments ? helpers().temporal.get() : helpers().row_temporal.get();
  return Ref::steal(PyObject_CallFunction(f, "iIIIIIIIi", static_cast<int>(tag), year, month, day, hour, minute,
                                          second, micro, neg ? 1 : 0));
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
      return host_text(r.str());
    case Tag::kBytes: {
      const auto n = r.get<uint32_t>();
      auto s = r.take(n);
      return Ref::steal(PyBytes_FromStringAndSize(reinterpret_cast<const char *>(s.data()), Py_ssize_t(s.size())));
    }
    case Tag::kDecimal:
      return call(value_rules() == ValueRules::kRowsNumber ? helpers().number.get() : helpers().decimal.get(),
                  host_text(r.str()).get());
    case Tag::kJson:
      return call(helpers().json_loads.get(), host_text(r.str()).get());
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
      return temporal(tag, r);
    case Tag::kTimestamp:
      // An instant (§7.2): Python makes the datetime, in the zone of OPTIONS.
      return Ref::steal(PyObject_CallFunction(helpers().timestamp.get(), "Li", static_cast<long long>(r.get<int64_t>()),
                                              value_rules() == ValueRules::kArguments ? 0 : 1));
    case Tag::kVector: {
      const auto dims = r.get<uint32_t>();
      r.align();
      const size_t pos = r.pos();
      r.take(size_t{dims} * 4);
      return vector != nullptr ? Ref::steal(PyObject_CallFunction(vector, "In", dims, Py_ssize_t(pos)))
                               : Ref::steal(PyObject_CallFunction(helpers().new_vector.get(), "OnI", body,
                                                                  Py_ssize_t(pos), dims));
    }
  }
  protocol_failure("an unknown value tag");
}

}  // namespace

Ref decode_value(Reader &r, PyObject *body, PyObject *vector) {
  return decode_tagged(static_cast<Tag>(r.get<uint8_t>()), r, body, vector);
}

void skip_value(Reader &r) {
  switch (static_cast<Tag>(r.get<uint8_t>())) {
    case Tag::kNull:
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
    case Tag::kI64:
    case Tag::kU64:
    case Tag::kF64:
    case Tag::kTimestamp:
      r.take(8);
      return;
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
      r.take(kCivilSize);
      return;
    case Tag::kVector: {
      const auto dims = r.get<uint32_t>();
      r.align();
      r.take(size_t{dims} * 4);
      return;
    }
  }
  protocol_failure("an unknown value tag");
}

// ------------------------------------------------------------------------------ module functions

/** setup(decimal, json_loads, temporal, new_vector, demote, row_temporal, number, timestamp) */
Ref py_setup(PyObject *args) {
  PyObject *d, *j, *t, *v, *m, *rt, *num, *ts;
  if (!PyArg_ParseTuple(args, "OOOOOOOO", &d, &j, &t, &v, &m, &rt, &num, &ts)) throw PyErrorSet{};
  if (PyDateTimeAPI == nullptr) {
    PyDateTime_IMPORT;
    if (PyDateTimeAPI == nullptr) throw PyErrorSet{};
  }
  Helpers &h = helpers();
  h.row_temporal = Ref::borrow(rt);
  h.timestamp = Ref::borrow(ts);
  h.number = Ref::borrow(num);
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
  // Every value has at least its tag (§1 item 1).
  if (n < 0 || size_t(n) > r.left()) protocol_failure("more values than the message holds");
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
  int rules = 0;
  if (!PyArg_ParseTuple(args, "OnnnO|i", &body, &pos, &nrows, &ncols, &row_type, &rules)) throw PyErrorSet{};
  const UseRules use(static_cast<ValueRules>(rules));
  if (row_type != Py_None &&
      (!PyType_Check(row_type) || !PyType_IsSubtype(reinterpret_cast<PyTypeObject *>(row_type), &PyTuple_Type))) {
    PyErr_SetString(PyExc_TypeError, "row_type must be a tuple subclass");
    throw PyErrorSet{};
  }
  Buffer buf(body, PyBUF_SIMPLE);
  Reader r(buf.bytes(), size_t(pos));
  if (nrows < 0 || ncols < 0 || (ncols != 0 && size_t(nrows) > r.left() / size_t(ncols)) || (ncols == 0 && nrows != 0))
    protocol_failure("more row values than the message holds");
  auto *type = reinterpret_cast<PyTypeObject *>(row_type);
  Ref list = Ref::steal(PyList_New(nrows));
  for (Py_ssize_t i = 0; i < nrows; i++) {
    Ref row = Ref::steal(row_type == Py_None ? PyTuple_New(ncols) : type->tp_alloc(type, ncols));
#if PY_VERSION_HEX >= 0x030E0000
    // Since 3.14 a tuple caches its hash, and -1 means "not computed": tp_alloc zero-fills, which
    // would make every row hash to 0 (tuple_subtype_new sets -1 as well).
    if (row_type != Py_None) reinterpret_cast<PyTupleObject *>(row.get())->ob_hash = -1;
#endif
    for (Py_ssize_t c = 0; c < ncols; c++) PyTuple_SET_ITEM(row.get(), c, decode_value(r, body, nullptr).release());
    PyList_SET_ITEM(list.get(), i, row.release());
  }
  return Ref::steal(Py_BuildValue("(Nn)", list.release(), Py_ssize_t(r.pos())));
}

namespace {

/** A large typed column (a VECTOR matrix: 300 MB for 100,000 VECTOR(768)) on 2 MiB pages.
    On 4 KiB pages every page of the fresh buffer faults on its own, about a third of
    fetch_columns' time; numpy asks the same for its own large arrays. A hint for the kernel's
    transparent huge pages (the distributions' "madvise" mode): errors are ignored. */
void advise_huge_pages(std::byte *p, size_t n) {
  constexpr uintptr_t kHuge = uintptr_t{2} << 20;
  if (n < 2 * kHuge) return;
  const auto begin = (reinterpret_cast<uintptr_t>(p) + kHuge - 1) & ~(kHuge - 1);
  const auto end = (reinterpret_cast<uintptr_t>(p) + n) & ~(kHuge - 1);
  if (end > begin) (void)madvise(reinterpret_cast<void *>(begin), end - begin, MADV_HUGEPAGE);
}

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
      advise_huge_pages(reinterpret_cast<std::byte *>(PyByteArray_AS_STRING(data.get())), want);
    }
    return reinterpret_cast<std::byte *>(PyByteArray_AS_STRING(data.get())) + size_t(count) * item_size;
  }
  /** The typed column becomes a list (Python: demote()); its values so far are converted. */
  void demote() {
    store();
    Ref r = Ref::steal(PyObject_CallOneArg(helpers().demote.get(), obj));
    load();
    if (mode != Mode::kList) {
      PyErr_SetString(PyExc_RuntimeError, "demote() left a typed column");
      throw PyErrorSet{};
    }
  }
};

bool add_typed(Column &c, Tag tag, Reader &r) {
  switch (c.mode) {
    case Mode::kVector: {
      if (tag != Tag::kVector) return false;
      const auto dims = r.get<uint32_t>();
      if (dims == 0 || (c.dims != 0 && dims != c.dims)) return false;
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
  int rules = 0;
  if (!PyArg_ParseTuple(args, "OnnO!|i", &body, &pos, &nrows, &PyList_Type, &collectors, &rules))
    throw PyErrorSet{};
  const UseRules use(static_cast<ValueRules>(rules));
  Buffer buf(body, PyBUF_SIMPLE);
  Reader r(buf.bytes(), size_t(pos));
  const Py_ssize_t ncols = PyList_GET_SIZE(collectors);
  if (nrows < 0 || (ncols != 0 && size_t(nrows) > r.left() / size_t(ncols)) || (ncols == 0 && nrows != 0))
    protocol_failure("more row values than the message holds");
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

// ------------------------------------------------------------------------------ parameters

namespace {

/** Appends n bytes to a bytearray; where they go. */
std::byte *grow(PyObject *buffer, size_t n) {
  const Py_ssize_t at = PyByteArray_GET_SIZE(buffer);
  if (PyByteArray_Resize(buffer, at + Py_ssize_t(n)) != 0) throw PyErrorSet{};
  return reinterpret_cast<std::byte *>(PyByteArray_AS_STRING(buffer)) + at;
}

template <class T>
void append(PyObject *buffer, Tag tag, T v) {
  std::byte *p = grow(buffer, 1 + sizeof(T));
  p[0] = static_cast<std::byte>(tag);
  std::memcpy(p + 1, &v, sizeof(T));
}

void append_text(PyObject *buffer, Tag tag, const char *s, size_t n) {
  std::byte *p = grow(buffer, 5 + n);
  p[0] = static_cast<std::byte>(tag);
  const auto len = static_cast<uint32_t>(n);
  std::memcpy(p + 1, &len, 4);
  std::memcpy(p + 5, s, n);
}

/** One value of a common type (PySql.kind's rules); false: the Python encoder takes it. */
bool encode_common(PyObject *buffer, PyObject *v) {
  if (v == Py_None) {
    *grow(buffer, 1) = static_cast<std::byte>(Tag::kNull);
    return true;
  }
  if (v == Py_True || v == Py_False) {
    append(buffer, Tag::kI64, int64_t{v == Py_True ? 1 : 0});
    return true;
  }
  if (PyLong_CheckExact(v)) {
    int overflow = 0;
    const long long x = PyLong_AsLongLongAndOverflow(v, &overflow);
    if (overflow == 0 && !(x == -1 && PyErr_Occurred())) {
      append(buffer, Tag::kI64, int64_t{x});
      return true;
    }
    PyErr_Clear();
    if (overflow > 0) {
      const unsigned long long u = PyLong_AsUnsignedLongLong(v);
      if (!(u == static_cast<unsigned long long>(-1) && PyErr_Occurred())) {
        append(buffer, Tag::kU64, uint64_t{u});
        return true;
      }
      PyErr_Clear();
    }
    return false;  // beyond 64 bits: DECIMAL text, by the Python encoder's rules
  }
  if (PyFloat_CheckExact(v)) {
    const double d = PyFloat_AS_DOUBLE(v);
    if (!std::isfinite(d)) return false;  // the Python encoder's TypeError
    append(buffer, Tag::kF64, d);
    return true;
  }
  if (PyUnicode_CheckExact(v)) {
    Py_ssize_t n;
    const char *s = PyUnicode_AsUTF8AndSize(v, &n);
    if (s == nullptr) {  // lone surrogates: the Python encoder repairs them
      PyErr_Clear();
      return false;
    }
    append_text(buffer, Tag::kString, s, size_t(n));
    return true;
  }
  if (PyBytes_CheckExact(v)) {
    append_text(buffer, Tag::kBytes, PyBytes_AS_STRING(v), size_t(PyBytes_GET_SIZE(v)));
    return true;
  }
  return false;
}

}  // namespace

Py_ssize_t encode_values(PyObject *buffer, PyObject *values, PyObject *fallback) {
  // A list is copied: the fallback runs guest code (__int__, a tzinfo, a dict subclass's items()),
  // which could change the guest's list and free the item array read here.
  Ref seq = Ref::steal(PyList_Check(values) ? PyList_AsTuple(values)
                                            : PySequence_Fast(values, "the parameters must be a sequence"));
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq.get());
  PyObject **items = PySequence_Fast_ITEMS(seq.get());
  for (Py_ssize_t i = 0; i < n; i++) {
    if (encode_common(buffer, items[i])) continue;
    Ref bytes = Ref::steal(
        PyObject_CallFunction(fallback, "On", items[i], PyByteArray_GET_SIZE(buffer)));
    const Buffer b(bytes.get(), PyBUF_SIMPLE);
    const auto src = b.bytes();
    std::memcpy(grow(buffer, src.size()), src.data(), src.size());
  }
  return n;
}

/** encode_values(buffer, values, fallback) -> count */
Ref py_encode_values(PyObject *args) {
  PyObject *buffer, *values, *fallback;
  if (!PyArg_ParseTuple(args, "O!OO", &PyByteArray_Type, &buffer, &values, &fallback)) throw PyErrorSet{};
  return Ref::steal(PyLong_FromSsize_t(encode_values(buffer, values, fallback)));
}

}  // namespace mle::cpython
