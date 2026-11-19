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

#include "module.hpp"

#include <cmath>
#include <exception>

#include "codec.hpp"
#include "mailbox.hpp"

namespace mle::cpython {

namespace w = mle::wire;

namespace {

/** open(spin) -> arena size */
Ref py_open(PyObject *args) {
  int spin = 2000;
  if (!PyArg_ParseTuple(args, "|i", &spin)) throw PyErrorSet{};
  try {
    return Ref::steal(PyLong_FromUnsignedLong(Mailbox::open(spin)));
  } catch (const std::exception &e) {
    PyErr_SetString(PyExc_OSError, e.what());
    throw PyErrorSet{};
  }
}

/** receive() -> (op, status, depth, call_seq, body): body is a read-only memoryview of the arena
    (one chunk: valid until the worker posts) or of a new bytes object (a reassembled message). */
Ref py_receive(PyObject *) {
  Ref assembled;
  const Message m = Mailbox::receive([&assembled](size_t total) {
    assembled = Ref::steal(PyBytes_FromStringAndSize(nullptr, Py_ssize_t(total)));
    return reinterpret_cast<std::byte *>(PyBytes_AS_STRING(assembled.get()));
  });
  Ref body = Ref::steal(
      m.in_arena ? PyMemoryView_FromMemory(reinterpret_cast<char *>(const_cast<std::byte *>(m.body.data())),
                                           Py_ssize_t(m.body.size()), PyBUF_READ)
                 : PyMemoryView_FromObject(assembled.get()));
  return Ref::steal(Py_BuildValue("(IIIKN)", m.op, m.status, m.depth, static_cast<unsigned long long>(m.call_seq),
                                  body.release()));
}

/** send(op, status, depth, call_seq, buffer) */
Ref py_send(PyObject *args) {
  unsigned op, status, depth;
  unsigned long long seq;
  PyObject *body;
  if (!PyArg_ParseTuple(args, "IIIKO", &op, &status, &depth, &seq, &body)) throw PyErrorSet{};
  const Buffer b(body, PyBUF_SIMPLE);
  Mailbox::send(op, status, depth, seq, b.bytes());
  return Ref::borrow(Py_None);
}

Ref py_pending(PyObject *) { return Ref::borrow(Mailbox::pending() ? Py_True : Py_False); }

/** stats(state, tainted[, heap_used]) */
Ref py_stats(PyObject *args) {
  unsigned state;
  int tainted;
  unsigned long long used = 0;
  if (!PyArg_ParseTuple(args, "Ip|K", &state, &tainted, &used)) throw PyErrorSet{};
  Mailbox::stats(state, tainted != 0, used);
  return Ref::borrow(Py_None);
}

/** begin(call_seq): a top-level request starts. */
Ref py_begin(PyObject *args) {
  unsigned long long seq;
  if (!PyArg_ParseTuple(args, "K", &seq)) throw PyErrorSet{};
  Interrupts::begin(seq);
  return Ref::borrow(Py_None);
}

/** end() -> whether the request was interrupted */
Ref py_end(PyObject *) { return Ref::borrow(Interrupts::end() ? Py_True : Py_False); }

/** The routine's value as the declared type, for the common cases only (exact int, float, str or
    None as an integer, floating-point or string type); false: Python's writer handles it, with its
    conversions and error messages. */
bool put_value(Writer &wr, PyObject *v, w::Tag tag, bool is_unsigned) {
  if (v == Py_None) return wr.put(static_cast<uint8_t>(w::Tag::kNull));
  switch (tag) {
    case w::Tag::kI64:
    case w::Tag::kU64: {
      if (!PyLong_CheckExact(v)) return false;
      if (is_unsigned || tag == w::Tag::kU64) {
        const unsigned long long u = PyLong_AsUnsignedLongLong(v);
        if (u == static_cast<unsigned long long>(-1) && PyErr_Occurred()) {
          PyErr_Clear();
          return false;
        }
        return wr.put(static_cast<uint8_t>(w::Tag::kU64)) && wr.put(static_cast<uint64_t>(u));
      }
      int overflow = 0;
      const long long x = PyLong_AsLongLongAndOverflow(v, &overflow);
      if (overflow != 0 || (x == -1 && PyErr_Occurred())) {
        PyErr_Clear();
        return false;
      }
      return wr.put(static_cast<uint8_t>(w::Tag::kI64)) && wr.put(static_cast<int64_t>(x));
    }
    case w::Tag::kF64:
    case w::Tag::kF32: {
      double d;
      if (PyFloat_CheckExact(v)) {
        d = PyFloat_AS_DOUBLE(v);
      } else if (PyLong_CheckExact(v)) {
        d = PyLong_AsDouble(v);
        if (d == -1.0 && PyErr_Occurred()) {
          PyErr_Clear();
          return false;
        }
      } else {
        return false;
      }
      if (tag == w::Tag::kF64) return wr.put(static_cast<uint8_t>(w::Tag::kF64)) && wr.put(d);
      const auto f = static_cast<float>(d);
      if (std::isfinite(d) && !std::isfinite(f)) return false;  // out of range: Python's error
      return wr.put(static_cast<uint8_t>(w::Tag::kF32)) && wr.put(f);
    }
    case w::Tag::kString: {
      if (!PyUnicode_CheckExact(v)) return false;
      Py_ssize_t n;
      const char *s = PyUnicode_AsUTF8AndSize(v, &n);
      if (s == nullptr) {  // lone surrogates: Python's writer replaces them
        PyErr_Clear();
        return false;
      }
      return wr.put(static_cast<uint8_t>(w::Tag::kString)) && wr.str({s, size_t(n)});
    }
    default:
      return false;
  }
}

/** result_fast(state, tainted, depth, seq, value, tag, unsigned, stdout, stderr) -> bool: an OK
    RESULT without OUT parameters written straight into the arena and posted (tag < 0: no return
    value); False if Python must write it (then nothing was sent). */
Ref py_result_fast(PyObject *args) {
  unsigned state, depth;
  int tainted, tag, is_unsigned;
  unsigned long long seq;
  PyObject *value, *out_obj, *err_obj;
  if (!PyArg_ParseTuple(args, "IpIKOipOO", &state, &tainted, &depth, &seq, &value, &tag, &is_unsigned, &out_obj,
                        &err_obj))
    throw PyErrorSet{};
  const Buffer out(out_obj, PyBUF_SIMPLE), err(err_obj, PyBUF_SIMPLE);
  Writer wr(Mailbox::arena());
  const auto raw = [&wr](std::span<const std::byte> b) {
    return wr.put(static_cast<uint32_t>(b.size())) && wr.bytes(b);
  };
  const bool ok = (tag < 0 ? wr.put(uint8_t{0})
                           : wr.put(uint8_t{1}) && put_value(wr, value, static_cast<w::Tag>(tag), is_unsigned != 0)) &&
                  wr.put(uint32_t{0}) &&                 // no OUT parameters
                  raw(out.bytes()) && raw(err.bytes()) &&  // console output
                  wr.put(uint8_t{0}) &&                  // console not cleared
                  wr.put(uint8_t{0});                    // no error
  if (!ok) return Ref::borrow(Py_False);
  Mailbox::stats(state, tainted != 0);
  Mailbox::post(uint32_t(w::Op::kResult), uint32_t(w::Status::kOk), depth, seq, uint32_t(wr.size()));
  return Ref::borrow(Py_True);
}

PyMethodDef kMethods[] = {
    {"open", boundary<py_open>, METH_VARARGS, "open(spin): map the mailbox; the arena's size"},
    {"receive", boundary<py_receive>, METH_NOARGS, "the next message: (op, status, depth, call_seq, body)"},
    {"send", boundary<py_send>, METH_VARARGS, "send(op, status, depth, call_seq, body)"},
    {"pending", boundary<py_pending>, METH_NOARGS, "whether a message is waiting"},
    {"stats", boundary<py_stats>, METH_VARARGS, "stats(state, tainted[, heap_used])"},
    {"begin", boundary<py_begin>, METH_VARARGS, "begin(call_seq): a top-level request starts"},
    {"end", boundary<py_end>, METH_NOARGS, "end(): whether the request was interrupted"},
    {"result_fast", boundary<py_result_fast>, METH_VARARGS, "an OK RESULT of a scalar, if it can"},
    {"setup", boundary<py_setup>, METH_VARARGS, "setup(decimal, json_loads, temporal, new_vector, demote)"},
    {"decode", boundary<py_decode>, METH_VARARGS, "decode(body, pos, n[, vector]) -> (values, pos)"},
    {"decode_rows", boundary<py_decode_rows>, METH_VARARGS, "decode_rows(body, pos, nrows, ncols, row_type)"},
    {"decode_columns", boundary<py_decode_columns>, METH_VARARGS, "decode_columns(body, pos, nrows, collectors)"},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_mlewire", "The MLE CPython worker's mailbox and codec.", -1,
                       kMethods, nullptr, nullptr, nullptr, nullptr};

}  // namespace

PyObject *init_mlewire() { return PyModule_Create(&kModule); }

}  // namespace mle::cpython
