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

#include <array>
#include <cmath>
#include <exception>
#include <unordered_map>

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

// ------------------------------------------------------------------------------ the call loop
//
// Top-level CALLs of simple routines (no OUT parameters, no USING) run here without Python's
// dispatcher (exp note 62 §8): arguments decoded, the function made with fresh globals and
// called, a scalar result written into the arena. Everything else, and every call whose routine
// printed, ran SQL or loaded a library (Python marks it), failed, was killed or returned a value
// the writer does not take, goes to Python as before.

struct FastRoutine {
  Ref fn;    // the function (entry form, mle_eval), or
  Ref code;  // the body's code object, called with a copy of
  Ref base;  // the globals it was prepared with (fresh globals per call)
  uint32_t nparams = 0;
  int ret_tag = -1;  // -1: no return value
  bool ret_unsigned = false;
  bool vectors = false;  // VECTOR parameters: reused numpy arrays (the dispatcher's vector_arg)
};

std::unordered_map<uint64_t, FastRoutine> g_fast;
/** Python did something during the call that its own tail must see (output, SQL, a library). */
bool g_marked = false;
bool g_tainted = false;

/** register(id, fn, code, base, nparams, ret_tag, unsigned, vectors) */
Ref py_register(PyObject *args) {
  unsigned long long id;
  PyObject *fn, *code, *base;
  unsigned nparams;
  int ret_tag, is_unsigned, vectors;
  if (!PyArg_ParseTuple(args, "KOOOIipp", &id, &fn, &code, &base, &nparams, &ret_tag, &is_unsigned, &vectors))
    throw PyErrorSet{};
  FastRoutine f;
  if (code != Py_None) {
    if (!PyCode_Check(code) || !PyDict_Check(base)) {
      PyErr_SetString(PyExc_TypeError, "register: code and a dict of globals, or a function");
      throw PyErrorSet{};
    }
    f.code = Ref::borrow(code);
    f.base = Ref::borrow(base);
  } else {
    f.fn = Ref::borrow(fn);
  }
  f.nparams = nparams;
  f.ret_tag = ret_tag;
  f.ret_unsigned = is_unsigned != 0;
  f.vectors = vectors != 0;
  g_fast.insert_or_assign(id, std::move(f));
  return Ref::borrow(Py_None);
}

/** unregister(id) or unregister(): every routine */
Ref py_unregister(PyObject *args) {
  unsigned long long id = 0;
  if (PyTuple_GET_SIZE(args) == 0) {
    g_fast.clear();
  } else {
    if (!PyArg_ParseTuple(args, "K", &id)) throw PyErrorSet{};
    g_fast.erase(id);
  }
  return Ref::borrow(Py_None);
}

Ref py_mark(PyObject *) {
  g_marked = true;
  return Ref::borrow(Py_None);
}

Ref py_taint(PyObject *args) {
  int t;
  if (!PyArg_ParseTuple(args, "p", &t)) throw PyErrorSet{};
  g_tainted = t != 0;
  return Ref::borrow(Py_None);
}

/** What the loop needs of the dispatcher. */
struct Loop {
  PyObject *handle;      // handle(op, depth, seq, body): every other request
  PyObject *finish;      // finish(id, result, exception, interrupted): the Python tail of a call
  PyObject *vector_arg;  // vector_arg(index, dims, body, pos)
  PyObject *state;       // the dispatcher's __dict__: seq, scope, scopes, routine
  PyObject *open;        // the libraries' loadsets being collected (cleared per top-level request)
  Ref k_seq, k_scope, k_scopes, k_routine;
};

void set(PyObject *dict, PyObject *key, Ref value) {
  if (PyDict_SetItem(dict, key, value.get()) != 0) throw PyErrorSet{};
}

/** The body as a Python object (a memoryview of the arena, or the reassembled bytes). */
Ref body_object(const Message &m, Ref &assembled) {
  if (!m.in_arena) return Ref::steal(PyMemoryView_FromObject(assembled.get()));
  return Ref::steal(PyMemoryView_FromMemory(reinterpret_cast<char *>(const_cast<std::byte *>(m.body.data())),
                                           Py_ssize_t(m.body.size()), PyBUF_READ));
}

/** Runs m as a fast call if its routine is registered; false: Python handles it (nothing done). */
bool fast_call(Loop &l, const Message &m, Ref &assembled) {
  Reader r(m.body, 0);
  const uint64_t id = r.get<uint64_t>();
  r.get<uint32_t>();  // events
  const uint32_t n = r.get<uint32_t>();
  const auto it = g_fast.find(id);
  if (it == g_fast.end() || it->second.nparams != n || n > 64) return false;
  const FastRoutine &f = it->second;

  // Arguments; anything Python reports (a zero date) is left to Python's path.
  std::array<Ref, 64> argv;
  Ref body;
  for (uint32_t i = 0; i < n; i++) {
    const Reader at = r;
    const auto tag = static_cast<w::Tag>(r.get<uint8_t>());
    if (tag == w::Tag::kVector) {
      if (!f.vectors) return false;
      const auto dims = r.get<uint32_t>();
      r.align();
      const size_t pos = r.pos();
      r.take(size_t{dims} * 4);
      if (!body) body = body_object(m, assembled);
      PyObject *v = PyObject_CallFunction(l.vector_arg, "IIOn", i, dims, body.get(), Py_ssize_t(pos));
      if (v == nullptr) {
        PyErr_Clear();
        return false;
      }
      argv[i] = Ref::steal(v);
      continue;
    }
    r = at;
    try {
      argv[i] = decode_value(r, nullptr, nullptr);
    } catch (const PyErrorSet &) {
      PyErr_Clear();
      return false;
    }
  }

  // The request's state, as the dispatcher's handle() sets it (the SQL API reads it).
  const Ref seq = Ref::steal(PyLong_FromUnsignedLongLong(m.call_seq));
  PyObject *scopes = PyDict_GetItemWithError(l.state, l.k_scopes.get());
  if (scopes == nullptr) {
    if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "the dispatcher has no scopes");
    throw PyErrorSet{};
  }
  const long long next = PyLong_AsLongLong(scopes) + 1;
  if (next == 0 && PyErr_Occurred()) throw PyErrorSet{};
  const Ref scope = Ref::steal(PyLong_FromLongLong(next));
  set(l.state, l.k_seq.get(), Ref::borrow(seq.get()));
  set(l.state, l.k_scopes.get(), Ref::borrow(scope.get()));
  set(l.state, l.k_scope.get(), Ref::borrow(scope.get()));
  const Ref rid = Ref::steal(PyLong_FromUnsignedLongLong(id));
  set(l.state, l.k_routine.get(), Ref::borrow(rid.get()));
  if (PyList_GET_SIZE(l.open) > 0 && PyList_SetSlice(l.open, 0, PY_SSIZE_T_MAX, nullptr) != 0) throw PyErrorSet{};

  Interrupts::begin(m.call_seq);
  Mailbox::stats(uint32_t(w::WorkerState::kRunning), g_tainted);
  g_marked = false;
  Ref fn = f.fn ? Ref::borrow(f.fn.get())
                : Ref::steal(PyFunction_New(f.code.get(), Ref::steal(PyDict_Copy(f.base.get())).get()));
  std::array<PyObject *, 64> raw;
  for (uint32_t i = 0; i < n; i++) raw[i] = argv[i].get();
  PyObject *ret = PyObject_Vectorcall(fn.get(), raw.data(), n, nullptr);
  Ref result = ret == nullptr ? Ref() : Ref::steal(ret);
  Ref failure = ret == nullptr ? Ref::steal(PyErr_GetRaisedException()) : Ref();
  set(l.state, l.k_routine.get(), Ref::steal(PyLong_FromLong(0)));
  const bool interrupted = Interrupts::end();

  if (result && !interrupted && !g_marked) {
    Writer wr(Mailbox::arena());
    const bool ok = (f.ret_tag < 0 ? wr.put(uint8_t{0})
                                   : wr.put(uint8_t{1}) && put_value(wr, result.get(), static_cast<w::Tag>(f.ret_tag),
                                                                     f.ret_unsigned)) &&
                    wr.put(uint32_t{0}) && wr.put(uint32_t{0}) && wr.put(uint32_t{0}) &&  // no OUTs, no output
                    wr.put(uint8_t{0}) && wr.put(uint8_t{0});
    if (ok) {
      Mailbox::stats(uint32_t(w::WorkerState::kIdle), g_tainted);
      Mailbox::post(uint32_t(w::Op::kResult), uint32_t(w::Status::kOk), 0, m.call_seq, uint32_t(wr.size()));
      return true;
    }
  }
  // Python's tail: the SQL API's end of call, the console, errors, other result types.
  PyObject *done = PyObject_CallFunction(l.finish, "OOOO", rid.get(), result ? result.get() : Py_None,
                                         failure ? failure.get() : Py_None, interrupted ? Py_True : Py_False);
  if (done != nullptr) {
    Py_DECREF(done);
  } else if (PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
    PyErr_Clear();  // a KILL after the call had ended
  } else {
    throw PyErrorSet{};
  }
  return true;
}

/** serve(handle, finish, vector_arg, state, open): the worker's request loop; returns only on error. */
Ref py_serve(PyObject *args) {
  Loop l{};
  if (!PyArg_ParseTuple(args, "OOOO!O!", &l.handle, &l.finish, &l.vector_arg, &PyDict_Type, &l.state, &PyList_Type,
                        &l.open))
    throw PyErrorSet{};
  l.k_seq = Ref::steal(PyUnicode_InternFromString("seq"));
  l.k_scope = Ref::steal(PyUnicode_InternFromString("scope"));
  l.k_scopes = Ref::steal(PyUnicode_InternFromString("scopes"));
  l.k_routine = Ref::steal(PyUnicode_InternFromString("routine"));
  while (true) {
    Ref assembled;
    const Message m = Mailbox::receive([&assembled](size_t total) {
      assembled = Ref::steal(PyBytes_FromStringAndSize(nullptr, Py_ssize_t(total)));
      return reinterpret_cast<std::byte *>(PyBytes_AS_STRING(assembled.get()));
    });
    // A KILL that came after its call had ended: taken here, not in the next call.
    if (PyErr_CheckSignals() != 0) PyErr_Clear();
    if (m.depth != 0) {
      PyErr_Format(PyExc_RuntimeError, "top-level message at depth %u", m.depth);
      throw PyErrorSet{};
    }
    if (m.op == uint32_t(w::Op::kCall) && fast_call(l, m, assembled)) continue;
    const Ref body = body_object(m, assembled);
    PyObject *r = PyObject_CallFunction(l.handle, "IIKO", m.op, m.depth, static_cast<unsigned long long>(m.call_seq),
                                        body.get());
    if (r != nullptr) {
      Py_DECREF(r);
    } else if (PyErr_ExceptionMatches(PyExc_KeyboardInterrupt)) {
      PyErr_Clear();  // a KILL after its call had ended
    } else {
      throw PyErrorSet{};
    }
  }
}

PyMethodDef kMethods[] = {
    {"serve", boundary<py_serve>, METH_VARARGS, "serve(handle, finish, vector_arg, state, open): the request loop"},
    {"register", boundary<py_register>, METH_VARARGS, "register(id, fn, code, base, nparams, ret_tag, unsigned, vectors)"},
    {"unregister", boundary<py_unregister>, METH_VARARGS, "unregister([id]): drop fast routines"},
    {"mark", boundary<py_mark>, METH_NOARGS, "the running call needs Python's tail"},
    {"taint", boundary<py_taint>, METH_VARARGS, "taint(flag): the worker asks to be retired"},
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
