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
  @file worker/cpython/codec.hpp

  The CPython worker's value codec (docs/protocol.md §7).

  Ref owns a PyObject reference; Reader and Writer read and write message bodies with bounds
  checks. A message of the host that breaks the protocol ends the process (common::
  protocol_failure, §1). Python errors are the C++ exception PyErrorSet inside the module (a
  Python exception is set) and become Python exceptions at the boundary (boundary<F>).
*/

#ifndef MLE_CPYTHON_CODEC_HPP
#define MLE_CPYTHON_CODEC_HPP

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "common/wire.hpp"
#include "mle/protocol.h"

namespace mle::cpython {

/** A Python exception is set; return nullptr to Python. */
struct PyErrorSet {};

using common::protocol_failure;
using common::Reader;
using common::Writer;

/** An owned PyObject reference (nullptr allowed). */
class Ref {
 public:
  Ref() = default;
  /** Takes over a new reference; nullptr means a Python error (throws). */
  static Ref steal(PyObject *p) {
    if (p == nullptr) throw PyErrorSet{};
    return Ref(p);
  }
  static Ref borrow(PyObject *p) {
    Py_XINCREF(p);
    return Ref(p);
  }
  Ref(Ref &&o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
  Ref &operator=(Ref &&o) noexcept {
    if (this != &o) {
      Py_XDECREF(m_p);
      m_p = std::exchange(o.m_p, nullptr);
    }
    return *this;
  }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  ~Ref() { Py_XDECREF(m_p); }

  PyObject *get() const { return m_p; }
  PyObject *release() { return std::exchange(m_p, nullptr); }
  explicit operator bool() const { return m_p != nullptr; }

 private:
  explicit Ref(PyObject *p) : m_p(p) {}
  PyObject *m_p = nullptr;
};

/** Text of the host as a str; ill-formed UTF-8 is repaired with U+FFFD (§7.1). */
Ref host_text(std::string_view s);

/** The Python exception that is set, as ASCII text ("" if none, or if it cannot be shown); the
    exception is cleared. */
std::string raised_text();

/** A Python object's buffer for the duration of a call. */
class Buffer {
 public:
  Buffer(PyObject *o, int flags) {
    if (PyObject_GetBuffer(o, &m_view, flags) != 0) throw PyErrorSet{};
  }
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  ~Buffer() { PyBuffer_Release(&m_view); }
  std::span<std::byte> bytes() const {
    return {static_cast<std::byte *>(m_view.buf), static_cast<size_t>(m_view.len)};
  }

 private:
  Py_buffer m_view{};
};

/** Python functions the codec calls for rare values (set by setup()). */
struct Helpers {
  Ref decimal;       // decimal.Decimal(str)
  Ref json_loads;    // json.loads(str)
  Ref temporal;      // temporal(tag, y, mo, d, h, mi, s, us, neg): errors (zero dates) as ConversionError
  Ref timestamp;     // timestamp(microseconds, rows): a TIMESTAMP as a datetime in the session's zone
  Ref new_vector;    // new_vector(body, pos, dims): a numpy array (copy)
  Ref demote;        // demote(collector): a typed column collector becomes a list
  Ref row_temporal;  // as temporal, for rows of SQL results: None for a zero date (PySql.temporal)
  Ref number;        // DECIMAL of rows with the decimalType NUMBER: float(str)
};
Helpers &helpers();

/** Which helpers values get: routine arguments (zero dates are errors), rows of SQL results
    (zero dates are None), or rows with DECIMAL as a number. */
enum class ValueRules : int { kArguments = 0, kRows = 1, kRowsNumber = 2 };

/** The rules decode_value() applies; set for a decode call by UseRules. */
ValueRules &value_rules();

class UseRules {
 public:
  explicit UseRules(ValueRules r) : m_saved(value_rules()) { value_rules() = r; }
  UseRules(const UseRules &) = delete;
  UseRules &operator=(const UseRules &) = delete;
  ~UseRules() { value_rules() = m_saved; }

 private:
  ValueRules m_saved;
};

/** Appends values (a sequence) to the SQL message in buffer (a bytearray) as statement parameters:
    the common types here, the others through fallback(value, offset) (the Python encoder: the
    bytes of one value at that offset of the message, VECTOR data 16-aligned). The count. */
Py_ssize_t encode_values(PyObject *buffer, PyObject *values, PyObject *fallback);

/** The next value of r as a Python object. vector(dims, pos) makes VECTORs if given. */
Ref decode_value(Reader &r, PyObject *body, PyObject *vector);
/** Skips the next value. */
void skip_value(Reader &r);

/** Calls F and turns C++ exceptions into Python ones. */
template <Ref (*F)(PyObject *)>
PyObject *boundary(PyObject *, PyObject *args) {
  try {
    return F(args).release();
  } catch (const PyErrorSet &) {
    return nullptr;
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  }
}

// Module functions (codec.cc).
Ref py_setup(PyObject *args);
Ref py_decode(PyObject *args);
Ref py_decode_rows(PyObject *args);
Ref py_decode_columns(PyObject *args);
Ref py_encode_values(PyObject *args);

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_CODEC_HPP
