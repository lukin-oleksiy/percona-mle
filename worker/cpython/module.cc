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

#include <malloc.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "codec.hpp"
#include "mailbox.hpp"

namespace mle::cpython {

namespace w = mle::wire;

Config &config() {
  static Config c;
  return c;
}

namespace {

// ------------------------------------------------------------------------------ requests

/** An SQL statement id that is not CLOSED (§10.1). */
struct Statement {
  enum class State : uint8_t { kIdle, kRows, kNext };
  State state = State::kIdle;
  /** Opened by PREPARE: its home is IDLE. Else by EXECUTE: its home is CLOSED. */
  bool prepared = false;
  /** PASS_RESULT_TO_CLIENT, given when the id was opened. */
  bool to_client = false;
  uint32_t param_count = 0;
  /** The columns of the current result set; of a prepared statement, the ones last received,
      which a later set's first batch may leave out (§10.3): (block, labels, tags). */
  Ref columns;
  uint32_t ncols = 0;
};

using Statements = std::unordered_map<uint64_t, Statement>;

/** A request being served, at its depth. */
struct Request {
  uint32_t op = 0;
  uint32_t depth = 0;
  /** A number of its own: statement handles and per-call options carry it. */
  uint64_t scope = 0;
  /** Guest code of this request may run: only then SQL and LIBRARY are sent (§10.1, §10.6). */
  bool guest = false;
  /** PREPARE, CALL, DROP: the routine. */
  uint64_t routine_id = 0;
  uint64_t library_epoch = 0;
  /** The statements of this request. */
  Statements statements;
  Request *outer = nullptr;
};

/** The innermost request being served; nullptr between requests. */
Request *g_request = nullptr;
/** call_seq of the top-level request being served or served last. */
uint64_t g_seq = 0;
uint64_t g_scopes = 0;
/** Statement ids are never used twice in a worker: unique among every request's (§10.1). */
uint64_t g_statement_ids = 0;

/** The request loop's Python side (serve()). */
struct Loop {
  PyObject *handle = nullptr;      // handle(op, body or None, head, ctx): a request; returns its outcome
  PyObject *finish = nullptr;      // finish(op, ctx, outcome, exception, delivered) -> (status, body)
  PyObject *convert = nullptr;     // convert(routine, value) -> the RESULT's values, for a fast call
  PyObject *failed = nullptr;      // failed(routine, exception) -> what a fast call failed with, described
  PyObject *vector_arg = nullptr;  // vector_arg(index, dims, body, pos)
};
Loop g_loop;

/** Python did something during the call that its own tail must see (SQL, a library). */
bool g_marked = false;
/** Guest code printed something no RESULT has carried yet. */
bool g_console = false;

std::byte *allocate(Ref &assembled, size_t total) {
  if ((config().message_max != 0 && total > config().message_max) || total > size_t{PY_SSIZE_T_MAX}) return nullptr;
  PyObject *bytes = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(total));
  if (bytes == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  assembled = Ref::steal(bytes);
  return reinterpret_cast<std::byte *>(PyBytes_AS_STRING(bytes));
}

Message receive(Ref &assembled) {
  return Mailbox::receive([&assembled](size_t total) { return allocate(assembled, total); });
}

/** The body as a Python object (a memoryview of the arena, or of the reassembled bytes). */
Ref body_object(const Message &m, Ref &assembled) {
  if (!m.in_arena) return Ref::steal(PyMemoryView_FromObject(assembled.get()));
  return Ref::steal(PyMemoryView_FromMemory(reinterpret_cast<char *>(const_cast<std::byte *>(m.body.data())),
                                           Py_ssize_t(m.body.size()), PyBUF_READ));
}

/** Text of the host as a str; ill-formed UTF-8 is repaired (§7.1). */
Ref str_of(std::string_view s) {
  return Ref::steal(PyUnicode_DecodeUTF8(s.data(), Py_ssize_t(s.size()), "replace"));
}

/** The guest code of the request served waits for the host: no interrupt is raised meanwhile.
    One that was delivered is raised by the caller when the guest code goes on. */
struct HostCall {
  HostCall() { guest_leave(); }
  ~HostCall() { guest_resume(); }
  HostCall(const HostCall &) = delete;
  HostCall &operator=(const HostCall &) = delete;
};

/** Why guest code may not send to the host now (sets the Python error), or false if it may: SQL
    and LIBRARY come only from the guest code of the innermost request, on the serving thread. */
bool host_unavailable(const char *what) {
  if (g_request != nullptr && g_request->guest && on_serving_thread()) return false;
  PyErr_Format(PyExc_RuntimeError, "%s only while a routine runs, in its own thread", what);
  return true;
}

// ------------------------------------------------------------------------------ RESULT

/** A RESULT of the worker itself, when its Python code could not make one: status INTERNAL with
    TAINTED, since the worker's state may be damaged (§9.4). text is ASCII. */
void post_internal(const Request &q, std::string_view text) {
  Mailbox::taint();
  Writer wr(Mailbox::arena());
  // No return value, OUT parameters, console output or findings; an Error without language,
  // frames or SQL error.
  (void)(wr.put<uint8_t>(0) && wr.put<uint32_t>(0) && wr.put<uint32_t>(0) && wr.put<uint32_t>(0) && wr.put<uint8_t>(0) &&
         wr.put<uint32_t>(0) && wr.put<uint8_t>(1) && wr.str("") && wr.str(text.substr(0, 1024)) && wr.put<uint32_t>(0) &&
         wr.put<uint32_t>(0) && wr.str("") && wr.put<uint64_t>(0));
  Mailbox::post(uint32_t(w::Op::kResult), uint32_t(w::Status::kInternal), q.depth, g_seq, uint32_t(wr.size()));
}

/** The Python error that is set, as ASCII text; the error is cleared. */
std::string python_error() {
  PyObject *e = PyErr_GetRaisedException();
  std::string text = "the worker's Python code failed";
  if (e != nullptr) {
    if (PyObject *shown = PyObject_ASCII(e); shown != nullptr) {
      if (const char *s = PyUnicode_AsUTF8(shown); s != nullptr) text = text + ": " + s;
      Py_DECREF(shown);
    }
    PyErr_Clear();
    Py_DECREF(e);
  }
  return text;
}

/** Posts the RESULT of q, which Python makes from what the request ended with. From here on no
    guest code of the request runs and the status is fixed (§11.1). */
void finish_request(Request &q, PyObject *ctx, PyObject *outcome, PyObject *failure, bool delivered) {
  const bool guest_op = q.op == uint32_t(w::Op::kPrepare) || q.op == uint32_t(w::Op::kCall);
  if (guest_op) g_console = false;  // this RESULT carries the console output
  PyObject *r = PyObject_CallFunction(g_loop.finish, "IOOOO", q.op, ctx, outcome != nullptr ? outcome : Py_None,
                                      failure != nullptr ? failure : Py_None, delivered ? Py_True : Py_False);
  // The statistics change when the worker becomes idle after a RESET, not on every request (§3.1).
  if (q.op == uint32_t(w::Op::kReset)) Mailbox::stats(uint32_t(w::WorkerState::kIdle));
  unsigned status = 0;
  PyObject *body = nullptr;
  Py_buffer view;
  if (r != nullptr && PyArg_ParseTuple(r, "IO", &status, &body) && PyObject_GetBuffer(body, &view, PyBUF_SIMPLE) == 0) {
    Mailbox::send(uint32_t(w::Op::kResult), status, q.depth, g_seq,
                  {static_cast<const std::byte *>(view.buf), static_cast<size_t>(view.len)});
    PyBuffer_Release(&view);
  } else {
    post_internal(q, python_error());
  }
  Py_XDECREF(r);
}

// ------------------------------------------------------------------------------ the fast call path
//
// Top-level CALLs of simple routines (no OUT parameters, no USING) run here without Python's
// dispatcher: arguments decoded, the function made with fresh globals and called, a scalar result
// written into the arena. Every call whose routine printed, ran SQL or loaded a library, failed or
// returned a value this writer does not take ends in Python (convert, finish).

struct FastRoutine {
  Ref routine;  // the dispatcher's Routine, for convert() and finish()
  Ref fn;       // the function (entry form, <eval>), or
  Ref code;     // the body's code object, called with a copy of
  Ref base;     // the globals it was prepared with (fresh globals per call)
  uint32_t nparams = 0;
  int ret_tag = -1;  // -1: no return value
  bool ret_unsigned = false;
  bool vectors = false;  // VECTOR parameters: reused numpy arrays (the dispatcher's vector_arg)
};

/** Shared with a running activation, which keeps its code when a nested PREPARE or DROP takes the
    routine away (§8.3). */
std::unordered_map<uint64_t, std::shared_ptr<const FastRoutine>> g_fast;

constexpr uint32_t kFastParams = 64;

/** The routine's value as the declared type, for the common cases only (exact int, float, str or
    None as an integer, floating-point or string type); false: Python's writer handles it, with its
    conversions and error messages. No guest code runs here. */
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
      if (!std::isfinite(d)) return false;  // not an SQL value: Python's error (§7.2)
      if (tag == w::Tag::kF64) return wr.put(static_cast<uint8_t>(w::Tag::kF64)) && wr.put(d);
      const auto f = static_cast<float>(d);
      if (!std::isfinite(f)) return false;  // out of range: Python's error
      return wr.put(static_cast<uint8_t>(w::Tag::kF32)) && wr.put(f);
    }
    case w::Tag::kString: {
      if (!PyUnicode_CheckExact(v)) return false;
      Py_ssize_t n;
      const char *s = PyUnicode_AsUTF8AndSize(v, &n);
      if (s == nullptr) {  // lone surrogates: Python's writer repairs them
        PyErr_Clear();
        return false;
      }
      return wr.put(static_cast<uint8_t>(w::Tag::kString)) && wr.str({s, size_t(n)});
    }
    default:
      return false;
  }
}

/** An OK RESULT of a CALL without OUT parameters, console output or findings, into the arena;
    its size, or 0 if this writer does not take the value or the arena is too small. */
uint32_t write_fast_result(const FastRoutine &f, PyObject *value) {
  Writer wr(Mailbox::arena());
  const bool ok = (f.ret_tag < 0 ? wr.put(uint8_t{0})
                                 : wr.put(uint8_t{1}) && put_value(wr, value, static_cast<w::Tag>(f.ret_tag), f.ret_unsigned)) &&
                  wr.put(uint32_t{0}) &&                         // no OUT parameters
                  wr.put(uint32_t{0}) && wr.put(uint32_t{0}) &&  // stdout, stderr
                  wr.put(uint8_t{0}) &&                          // console not cleared
                  wr.put(uint32_t{0}) &&                         // no findings
                  wr.put(uint8_t{0});                            // no error
  return ok ? uint32_t(wr.size()) : 0;
}

/** Runs the top-level CALL m of q as a fast call if its routine is registered; false: Python
    handles it (nothing was done). */
bool fast_call(Request &q, const Message &m, Ref &assembled) {
  const auto it = g_fast.find(q.routine_id);
  if (it == g_fast.end()) return false;
  const std::shared_ptr<const FastRoutine> keep = it->second;
  const FastRoutine &f = *keep;
  Reader r(m.body, 16);
  // Another argument count than the routine's Params is Python's to find (a protocol failure).
  if (r.get<uint32_t>() != f.nparams) return false;
  const uint32_t n = f.nparams;

  // Arguments; anything Python reports (a zero date) is left to Python's path.
  std::array<Ref, kFastParams> argv;
  Ref body;
  for (uint32_t i = 0; i < n; i++) {
    const Reader at = r;
    const auto tag = static_cast<w::Tag>(r.get<uint8_t>());
    try {
      if (tag == w::Tag::kVector) {
        if (!f.vectors) return false;
        const auto dims = r.get<uint32_t>();
        r.align();
        const size_t pos = r.pos();
        r.take(size_t{dims} * 4);
        if (!body) body = body_object(m, assembled);
        argv[i] = Ref::steal(PyObject_CallFunction(g_loop.vector_arg, "IIOn", i, dims, body.get(), Py_ssize_t(pos)));
      } else {
        r = at;
        argv[i] = decode_value(r, nullptr, nullptr);
      }
    } catch (const PyErrorSet &) {
      PyErr_Clear();
      return false;
    }
  }

  g_marked = false;
  // The function with fresh globals (its errors, MemoryError, are the call's, as Python's own).
  PyObject *ret = nullptr;
  q.guest = true;
  guest_enter();
  PyObject *globals = f.fn ? nullptr : PyDict_Copy(f.base.get());
  PyObject *made = f.fn || globals == nullptr ? nullptr : PyFunction_New(f.code.get(), globals);
  Py_XDECREF(globals);
  if (f.fn || made != nullptr) {
    std::array<PyObject *, kFastParams> raw;
    for (uint32_t i = 0; i < n; i++) raw[i] = argv[i].get();
    ret = PyObject_Vectorcall(f.fn ? f.fn.get() : made, raw.data(), n, nullptr);
  }
  Py_XDECREF(made);
  // The RESULT straight into the arena, or its values by Python's writer, whose conversions can
  // run guest code: the request is not over for interrupts before they are done (§4.1).
  uint32_t size = 0;
  PyObject *values = nullptr;
  if (ret != nullptr) {
    if (!g_marked && !g_console) size = write_fast_result(f, ret);
    if (size == 0) values = PyObject_CallFunctionObjArgs(g_loop.convert, f.routine.get(), ret, nullptr);
    Py_DECREF(ret);
  } else {
    // What it failed with is described while the request can still be interrupted: an
    // exception's text is guest code.
    PyObject *raised = PyErr_GetRaisedException();
    values = PyObject_CallFunctionObjArgs(g_loop.failed, f.routine.get(), raised, nullptr);
    Py_XDECREF(raised);
  }
  guest_leave();
  q.guest = false;
  PyObject *failure = size == 0 && values == nullptr ? PyErr_GetRaisedException() : nullptr;
  const bool delivered = Interrupts::end();
  if (size != 0) {
    // The code completed before an interrupt could stop it: its result stands (§11.1).
    Mailbox::post(uint32_t(w::Op::kResult), uint32_t(w::Status::kOk), 0, g_seq, size);
    return true;
  }
  PyObject *ctx = PyList_New(1);
  if (ctx == nullptr) {
    post_internal(q, python_error());
  } else {
    PyList_SET_ITEM(ctx, 0, Py_NewRef(f.routine.get()));
    finish_request(q, ctx, values, failure, delivered);
    Py_DECREF(ctx);
  }
  Py_XDECREF(values);
  Py_XDECREF(failure);
  return true;
}

/** The routine id is not prepared any more, or is prepared anew (§8.3): later CALLs do not find
    its fast routine. */
void forget_routine(uint64_t id) {
  g_fast.erase(id);
}

/** Serves the request m: from its complete receipt to the post of its RESULT. Nested requests
    come here from the wait for an SQL_RESULT (§10.6). */
void dispatch(const Message &m, Ref &assembled) {
  const auto op = static_cast<w::Op>(m.op);
  const bool top = m.depth == 0;
  const bool guest_op = op == w::Op::kPrepare || op == w::Op::kCall;
  const bool interruptible = guest_op || op == w::Op::kParseLibrary;
  Request q;
  q.op = m.op;
  q.depth = m.depth;
  q.scope = ++g_scopes;
  q.outer = g_request;
  if (guest_op || op == w::Op::kDrop) q.routine_id = m.discarded ? m.head : Reader(m.body).get<uint64_t>();
  if (op == w::Op::kCall && !m.discarded) q.library_epoch = Reader(m.body, 8).get<uint64_t>();
  if (op == w::Op::kPrepare || op == w::Op::kDrop) forget_routine(q.routine_id);
  if (op == w::Op::kReset) {
    g_fast.clear();
  }
  if (top) {
    // The worker begins to serve the request: it has received it completely (§11.1).
    g_seq = m.call_seq;
    Interrupts::begin(g_seq, interruptible);
  }
  g_request = &q;
  // g_request is the outer request's again however this one ends (MVP review CPY-7).
  struct Restore {
    Request *outer;
    ~Restore() { g_request = outer; }
  } restore{q.outer};
  if (!(op == w::Op::kCall && top && !m.discarded && fast_call(q, m, assembled))) {
    PyObject *ctx = PyList_New(0);
    PyObject *body = nullptr;
    if (ctx != nullptr) {
      // A body that cannot be made (MemoryError) is this request's INTERNAL below, not an
      // exception out of the request loop with no RESULT (MVP review CPY-7).
      try {
        body = m.discarded ? Py_NewRef(Py_None) : body_object(m, assembled).release();
      } catch (const PyErrorSet &) {
        body = nullptr;
      }
    }
    PyObject *outcome = nullptr;
    if (body != nullptr) {
      q.guest = guest_op && !m.discarded;
      if (q.guest) guest_enter();
      outcome = PyObject_CallFunction(g_loop.handle, "IOKO", m.op, body, static_cast<unsigned long long>(m.head), ctx);
      if (q.guest) guest_leave();
      q.guest = false;
    }
    PyObject *failure = outcome == nullptr ? PyErr_GetRaisedException() : nullptr;
    // The status is fixed from here on. An interrupt of the top-level request that was delivered
    // and not raised is cancelled; inside a nested request it stays for the outer code.
    const bool delivered = interruptible && (top ? Interrupts::end() : Interrupts::interrupted());
    if (ctx == nullptr || body == nullptr) {
      PyErr_SetRaisedException(Py_XNewRef(failure));
      post_internal(q, python_error());
    } else {
      finish_request(q, ctx, outcome, failure, delivered);
    }
    Py_XDECREF(outcome);
    Py_XDECREF(failure);
    Py_XDECREF(body);
    Py_XDECREF(ctx);
  }
}

/** serve(handle, finish, convert, failed, vector_arg): the worker's request loop; never returns. */
Ref py_serve(PyObject *args) {
  if (!PyArg_ParseTuple(args, "OOOOO", &g_loop.handle, &g_loop.finish, &g_loop.convert, &g_loop.failed,
                        &g_loop.vector_arg))
    throw PyErrorSet{};
  serving_thread();
  while (true) {
    Ref assembled;
    const Message m = receive(assembled);
    dispatch(m, assembled);
  }
}

/** register(id, routine, fn, code, base, nparams, ret_tag, unsigned, vectors) */
Ref py_register(PyObject *args) {
  unsigned long long id;
  PyObject *routine, *fn, *code, *base;
  unsigned nparams;
  int ret_tag, is_unsigned, vectors;
  if (!PyArg_ParseTuple(args, "KOOOOIipp", &id, &routine, &fn, &code, &base, &nparams, &ret_tag, &is_unsigned, &vectors))
    throw PyErrorSet{};
  auto f = std::make_shared<FastRoutine>();
  if (code != Py_None) {
    if (!PyCode_Check(code) || !PyDict_Check(base)) {
      PyErr_SetString(PyExc_TypeError, "register: code and a dict of globals, or a function");
      throw PyErrorSet{};
    }
    f->code = Ref::borrow(code);
    f->base = Ref::borrow(base);
  } else {
    f->fn = Ref::borrow(fn);
  }
  if (nparams > kFastParams) {
    PyErr_SetString(PyExc_ValueError, "register: too many parameters");
    throw PyErrorSet{};
  }
  f->routine = Ref::borrow(routine);
  f->nparams = nparams;
  f->ret_tag = ret_tag;
  f->ret_unsigned = is_unsigned != 0;
  f->vectors = vectors != 0;
  g_fast.insert_or_assign(id, std::move(f));
  return Ref::borrow(Py_None);
}

/** unregister(id): the routine is not one of the fast call path any more. */
Ref py_unregister(PyObject *args) {
  unsigned long long id;
  if (!PyArg_ParseTuple(args, "K", &id)) throw PyErrorSet{};
  g_fast.erase(id);
  return Ref::borrow(Py_None);
}

Ref py_mark(PyObject *) {
  g_marked = true;
  return Ref::borrow(Py_None);
}

Ref py_printed(PyObject *) {
  g_console = true;
  return Ref::borrow(Py_None);
}

/** interrupt_due() -> whether the worker's SIGUSR1 handler raises KeyboardInterrupt (D11). */
Ref py_interrupt_due(PyObject *) { return Ref::borrow(interrupt_due() ? Py_True : Py_False); }

/** interrupt_signal(installed): installed() tells whether the worker's SIGUSR1 handler is set. */
Ref py_interrupt_signal(PyObject *args) {
  PyObject *installed = nullptr;
  if (!PyArg_ParseTuple(args, "O", &installed)) throw PyErrorSet{};
  interrupt_signal(installed == Py_None ? nullptr : installed);
  return Ref::borrow(Py_None);
}

/** guest_pause() -> whether guest code was running: the worker's own bookkeeping follows, where
    no KeyboardInterrupt may land (MVP review CPY-6); guest_unpause(was) ends it. */
Ref py_guest_pause(PyObject *) {
  const bool was = guest_running();
  if (was) guest_leave();
  return Ref::borrow(was ? Py_True : Py_False);
}

Ref py_guest_unpause(PyObject *args) {
  int was = 0;
  if (!PyArg_ParseTuple(args, "p", &was)) throw PyErrorSet{};
  if (was) guest_resume();
  return Ref::borrow(Py_None);
}

/** guest_exec(code, globals, guest): exec of a library's code as guest code when guest is true,
    between paused bookkeeping: interrupts only inside it (MVP review CPY-6). */
Ref py_guest_exec(PyObject *args) {
  PyObject *code = nullptr, *globals = nullptr;
  int guest = 0;
  if (!PyArg_ParseTuple(args, "OO!p", &code, &PyDict_Type, &globals, &guest)) throw PyErrorSet{};
  if (guest) guest_resume();
  PyObject *r = PyEval_EvalCode(code, globals, globals);
  if (guest) guest_leave();
  return Ref::steal(r);
}

/** trim(): free memory back to the system after RESET (§9.7 SHOULD; MVP review CPY-11). */
Ref py_trim(PyObject *) {
  malloc_trim(0);
  return Ref::borrow(Py_None);
}

Ref py_taint(PyObject *) {
  Mailbox::taint();
  return Ref::borrow(Py_None);
}

/** request() -> (depth, scope, routine id, library epoch, call_seq, whether a CALL) of the
    innermost request being served, or None between requests. */
Ref py_request(PyObject *) {
  const Request *q = g_request;
  if (q == nullptr) return Ref::borrow(Py_None);
  return Ref::steal(Py_BuildValue("(IKKKKO)", q->depth, static_cast<unsigned long long>(q->scope),
                                  static_cast<unsigned long long>(q->routine_id),
                                  static_cast<unsigned long long>(q->library_epoch),
                                  static_cast<unsigned long long>(g_seq),
                                  q->op == uint32_t(w::Op::kCall) ? Py_True : Py_False));
}

/** epoch(library_epoch): of the PREPARE being served (the field ends its body). */
Ref py_epoch(PyObject *args) {
  unsigned long long epoch;
  if (!PyArg_ParseTuple(args, "K", &epoch)) throw PyErrorSet{};
  if (g_request != nullptr) g_request->library_epoch = epoch;
  return Ref::borrow(Py_None);
}

Ref py_arena_size(PyObject *) { return Ref::steal(PyLong_FromUnsignedLong(config().arena_size)); }

/** ready(info, runtime): READY with status OK (§9.1), and the statistics of an idle worker. */
Ref py_ready(PyObject *args) {
  const char *info, *runtime;
  if (!PyArg_ParseTuple(args, "ss", &info, &runtime)) throw PyErrorSet{};
  const std::string_view languages[] = {"python"};
  Mailbox::stats(uint32_t(w::WorkerState::kIdle));
  Mailbox::ready(info, "cpython", languages, runtime);
  return Ref::borrow(Py_None);
}

/** protocol_failure(text): a message of the host breaks the protocol; the process ends (§1). */
Ref py_protocol_failure(PyObject *args) {
  const char *text;
  if (!PyArg_ParseTuple(args, "s", &text)) throw PyErrorSet{};
  protocol_failure(text);
}

// ------------------------------------------------------------------------------ SQL round trips
//
// The SQL API's mechanics (§10): a statement message is checked, finished and sent, the host's
// nested requests are served while the worker waits, and the reply is checked against the
// statement's state (§10.1), which it moves, and parsed into a tuple. Everything of a message
// that guest code could get wrong is decided here (§10.6). The API (sql.py) is Python.

/** The reply to the SQL message just sent, nested requests served before it. */
Message sql_reply(Ref &assembled) {
  while (true) {
    assembled = Ref();
    const Message m = receive(assembled);
    if (m.op == uint32_t(w::Op::kSqlResult)) return m;
    // A nested PREPARE, CALL or DROP (the mailbox has checked that it is one), served on this
    // thread from inside the wait (§10.6).
    dispatch(m, assembled);
  }
}

/** A list of Warning: None if empty, else a list of (level, code, message). */
Ref warnings(Reader &r) {
  const uint32_t n = r.count(w::kMaxWarnings, 12);
  if (n == 0) return Ref::borrow(Py_None);
  Ref list = Ref::steal(PyList_New(n));
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t level = r.get<uint32_t>(), code = r.get<uint32_t>();
    Ref msg = str_of(r.str());
    PyList_SET_ITEM(list.get(), i, Ref::steal(Py_BuildValue("(IIN)", level, code, msg.release())).release());
  }
  return list;
}

/** A list of Column of count entries as (block, labels, tags): the columns' bytes as on the wire
    (made into Column objects only when a routine asks for them), their labels and type tags. */
Ref columns(Reader &r, uint32_t count) {
  const size_t block_at = r.pos();
  Ref labels = Ref::steal(PyTuple_New(count));
  Ref tags = Ref::steal(PyTuple_New(count));
  for (uint32_t i = 0; i < count; i++) {
    PyTuple_SET_ITEM(labels.get(), i, str_of(r.str()).release());
    r.str();  // table
    const auto tag = r.get<uint8_t>();
    if (tag > w::kMaxTag) protocol_failure("an unknown tag in a Column's Type");
    PyTuple_SET_ITEM(tags.get(), i, Ref::steal(PyLong_FromLong(tag)).release());
    r.take(13);                            // Type: unsigned, length, precision, scale
    for (int k = 0; k < 6; k++) r.str();   // type_name, org_name, schema, org_table, charset, collation
    r.get<uint32_t>();                     // flags
  }
  const auto block = r.all().subspan(block_at, r.pos() - block_at);
  return Ref::steal(Py_BuildValue("(y#OO)", reinterpret_cast<const char *>(block.data()), Py_ssize_t(block.size()),
                                  labels.get(), tags.get()));
}

/** What an SQL message of the worker was, for the check of its reply. */
struct Sent {
  w::SqlAction action = w::SqlAction::kExecute;
  uint64_t id = 0;
  bool to_client = false;
  /** EXECUTE_MANY: the rows sent. */
  uint32_t rows = 0;
};

Ref ok_tuple(const Request &q, uint64_t affected, uint64_t insert_id, Ref warns, bool more_results, uint32_t params,
             uint64_t exec_us, uint64_t id) {
  return Ref::steal(Py_BuildValue("(iKKNOIKKK)", 1, static_cast<unsigned long long>(affected),
                                  static_cast<unsigned long long>(insert_id), warns.release(),
                                  more_results ? Py_True : Py_False, params, static_cast<unsigned long long>(exec_us),
                                  static_cast<unsigned long long>(id), static_cast<unsigned long long>(q.scope)));
}

/** An ERROR the worker answers itself, without a message: it has no error id. */
Ref error_tuple(uint32_t code, std::string_view state, std::string_view message) {
  return Ref::steal(
      Py_BuildValue("(iINNOKK)", 0, code, str_of(state).release(), str_of(message).release(), Py_None, 0ULL, 0ULL));
}

/** Checks the SQL_RESULT m that answers the message `sent` of q (§1: the reply kind for the
    action, counts against the bytes, the numbers the worker finds its state by), moves the id to
    its next state (§10.1), and returns the reply:
      (0, errno, sqlstate, message, failed row or None, error id, call_seq)
      (1, affected, insert id, warnings, more results, parameter count, exec us, id, scope)
      (2, columns or None, total rows or None, body, rows position, rows, more, more results,
       warnings, exec us, id, scope)
      (3,)
    id and scope are the statement's handle for later actions.
    columns: (block, labels, tags) with the first batch of a result set. The rows stay in body
    and are decoded when a routine reads them. */
Ref sql_result(Request &q, const Sent &sent, const Message &m, Ref &assembled) {
  using State = Statement::State;
  const w::SqlAction action = sent.action;
  Reader r(m.body);
  const auto kind = r.get<uint8_t>();
  if (kind > w::kMaxSqlResultKind) protocol_failure("an unknown SQL_RESULT kind");
  const auto known = q.statements.find(sent.id);
  Statement *st = known != q.statements.end() ? &known->second : nullptr;  // nullptr: CLOSED
  const bool to_client = st != nullptr ? st->to_client : sent.to_client;
  const bool nothing_next = action == w::SqlAction::kNextResult && (st == nullptr || st->state == State::kIdle);
  bool legal = false;
  switch (static_cast<w::SqlResultKind>(kind)) {
    case w::SqlResultKind::kError:
      legal = action != w::SqlAction::kClose;
      break;
    case w::SqlResultKind::kOk:
      legal = action != w::SqlAction::kFetch && !nothing_next;
      break;
    case w::SqlResultKind::kRows:
      legal = !to_client && !nothing_next && action != w::SqlAction::kPrepare &&
              action != w::SqlAction::kExecuteMany && action != w::SqlAction::kClose;
      break;
    case w::SqlResultKind::kEnd:
      legal = action == w::SqlAction::kNextResult;
      break;
  }
  if (!legal) protocol_failure("an SQL_RESULT of a kind that cannot answer the action in the statement's state");
  // The id's home: IDLE for a prepared statement, CLOSED for a direct one.
  const auto home = [&]() {
    if (st == nullptr) return;
    if (st->prepared)
      st->state = State::kIdle;
    else
      q.statements.erase(sent.id);
    st = nullptr;
  };
  // The id has a result to read: a direct statement gets its entry now.
  const auto open = [&]() -> Statement & {
    if (st == nullptr) {
      st = &q.statements[sent.id];
      st->to_client = to_client;
    }
    return *st;
  };
  switch (static_cast<w::SqlResultKind>(kind)) {
    case w::SqlResultKind::kError: {
      const uint32_t code = r.get<uint32_t>();
      Ref state = str_of(r.str()), message = str_of(r.str());
      const uint32_t row = r.get<uint32_t>();
      const uint64_t error_id = r.get<uint64_t>();
      Ref failed = Ref::borrow(Py_None);
      if (action == w::SqlAction::kExecuteMany) {
        if (row > sent.rows) protocol_failure("a failed_row beyond the rows sent");
        failed = Ref::steal(PyLong_FromUnsignedLong(row));
      }
      // The actions sent are allowed in the id's state (the others fail without a message), so
      // rule 3 applies: the id goes home.
      home();
      return Ref::steal(Py_BuildValue("(iINNNKK)", 0, code, state.release(), message.release(), failed.release(),
                                      static_cast<unsigned long long>(error_id), static_cast<unsigned long long>(g_seq)));
    }
    case w::SqlResultKind::kOk: {
      const uint64_t affected = r.get<uint64_t>(), insert_id = r.get<uint64_t>();
      Ref warns = warnings(r);
      const bool more_results = r.boolean();
      const uint32_t params = r.get<uint32_t>();
      const uint64_t exec_us = r.get<uint64_t>();
      if (action == w::SqlAction::kClose) {
        if (st != nullptr) q.statements.erase(sent.id);
      } else if (action == w::SqlAction::kPrepare) {
        Statement &s = q.statements[sent.id];
        s.prepared = true;
        s.to_client = sent.to_client;
        s.param_count = params;
      } else if (more_results) {
        open().state = State::kNext;
      } else {
        home();
      }
      return ok_tuple(q, affected, insert_id, std::move(warns), more_results, params, exec_us, sent.id);
    }
    case w::SqlResultKind::kRows: {
      // The first batch of a result set carries its columns; of a prepared statement it may leave
      // them out when they are the ones it sent last. A later batch has none (§10.3).
      const bool first = action != w::SqlAction::kFetch;
      const uint32_t ncols = r.count(w::kMaxColumns, 50);
      Ref cols;
      if (ncols != 0) {
        cols = columns(r, ncols);
      } else if (st == nullptr || !st->columns || (first && !st->prepared)) {
        protocol_failure("a batch of rows without columns for a statement that has none");
      }
      const uint32_t set_cols = ncols != 0 ? ncols : st->ncols;
      const uint64_t total = r.get<uint64_t>();
      const uint32_t nrows = r.get<uint32_t>();
      // Every value has at least its tag.
      if (uint64_t{nrows} * set_cols > r.left()) protocol_failure("more row values than the message holds");
      const size_t rows_at = r.pos();
      for (uint64_t i = 0; i < uint64_t{nrows} * set_cols; i++) skip_value(r);
      const bool more = r.boolean(), more_results = r.boolean();
      Ref warns = warnings(r);
      const uint64_t exec_us = r.get<uint64_t>();
      // The reply tells the columns with a set's first batch.
      Ref shown = Ref::borrow(Py_None);
      if (more || more_results || (st != nullptr && st->prepared)) {
        Statement &s = open();
        if (ncols != 0) {
          s.columns = std::move(cols);
          s.ncols = ncols;
        }
        if (first) shown = Ref::borrow(s.columns.get());
        s.state = more ? State::kRows : more_results ? State::kNext : State::kIdle;
      } else {
        // A direct statement's set is complete and no result follows: CLOSED.
        if (first) shown = std::move(cols);
        home();
      }
      Ref body = nrows == 0 ? Ref::borrow(Py_None) : body_object(m, assembled);
      Ref total_rows =
          total == w::kUnknownTotalRows ? Ref::borrow(Py_None) : Ref::steal(PyLong_FromUnsignedLongLong(total));
      return Ref::steal(Py_BuildValue("(iONNnIOONKKK)", 2, shown.get(), total_rows.release(), body.release(),
                                      Py_ssize_t(rows_at), nrows, more ? Py_True : Py_False,
                                      more_results ? Py_True : Py_False, warns.release(),
                                      static_cast<unsigned long long>(exec_us), static_cast<unsigned long long>(sent.id),
                                      static_cast<unsigned long long>(q.scope)));
    }
    case w::SqlResultKind::kEnd:
      home();
      return Ref::steal(Py_BuildValue("(i)", 3));
  }
  protocol_failure("an unknown SQL_RESULT kind");
}

/** Sends CLOSE for id and takes its OK (§10.1 rule 1): the id is CLOSED. */
void sql_close(Request &q, uint64_t id) {
  std::array<std::byte, w::kSqlCloseSize> close{};
  Writer wr(close);
  (void)(wr.put(uint8_t(w::SqlAction::kClose)) && wr.put(id));  // no text, no params, flags and the rest 0
  Mailbox::send(uint32_t(w::Op::kSql), 0, q.depth, g_seq, close);
  Ref assembled;
  const Message m = sql_reply(assembled);
  if (m.discarded || m.body.empty() || m.body[0] != std::byte{uint8_t(w::SqlResultKind::kOk)})
    protocol_failure("a reply to CLOSE that is not OK");
  q.statements.erase(id);
}

/** sql(action, scope, id, message, count, flags, width) -> the reply tuple (sql_result), or None
    if the message would be larger than a worker's may be (nothing is sent).

    message: for EXECUTE, PREPARE, EXECUTE_PREPARED and EXECUTE_MANY a bytearray of the SQL message
    up to its values (9 bytes for the action and the id, the text, 4 bytes for the value count,
    the values), which this function completes; else None. scope and id: the statement's handle
    as an earlier reply gave it (the reply's id, the scope of the request that got it); not used
    by EXECUTE and PREPARE, which open a new id. flags: PASS_RESULT_TO_CLIENT, with EXECUTE and
    PREPARE.

    What does not need the host is answered here: an id that is CLOSED (CLOSE: OK; NEXT_RESULT:
    END; others: an ERROR), FETCH without rows to fetch. The handle of another request fails. */
Ref py_sql(PyObject *args) {
  using State = Statement::State;
  unsigned action_code, count, flags, width;
  unsigned long long scope, handle;
  PyObject *message;
  if (!PyArg_ParseTuple(args, "IKKOIII", &action_code, &scope, &handle, &message, &count, &flags, &width))
    throw PyErrorSet{};
  if (host_unavailable("SQL is available")) throw PyErrorSet{};
  Request &q = *g_request;
  if (action_code > w::kMaxSqlAction) {
    PyErr_SetString(PyExc_ValueError, "sql: unknown action");
    throw PyErrorSet{};
  }
  Sent sent;
  sent.action = static_cast<w::SqlAction>(action_code);
  const w::SqlAction action = sent.action;
  const bool opens = action == w::SqlAction::kExecute || action == w::SqlAction::kPrepare;
  const bool with_message = opens || action == w::SqlAction::kExecutePrepared || action == w::SqlAction::kExecuteMany;
  // After an interrupt the host answers ERROR to everything but CLOSE; the worker does it itself,
  // without a message, and the guest code does not run on (§11.1).
  if (Interrupts::interrupted() && action != w::SqlAction::kClose) {
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    throw PyErrorSet{};
  }
  if (with_message ? !PyByteArray_Check(message) : message != Py_None) {
    PyErr_SetString(PyExc_TypeError, "sql: a bytearray for a statement with text or values, else None");
    throw PyErrorSet{};
  }
  if (count > w::kMaxValues || width > w::kMaxValues || (flags & ~unsigned{w::kPassResultToClient}) != 0) {
    PyErr_SetString(PyExc_ValueError, "sql: too many values, or unknown flags");
    throw PyErrorSet{};
  }

  Statement *st = nullptr;
  if (!opens) {
    // A statement belongs to the request that opened it: the handle of
    // another request, an earlier one or the caller's, fails here and nothing is sent.
    if (scope != q.scope) {
      PyErr_SetString(PyExc_RuntimeError,
                      "the SQL statement or result belongs to another routine call (one that ended, or the caller)");
      throw PyErrorSet{};
    }
    sent.id = handle;
    const auto known = q.statements.find(sent.id);
    st = known != q.statements.end() ? &known->second : nullptr;
    if (action == w::SqlAction::kClose) {
      if (st == nullptr) return ok_tuple(q, 0, 0, Ref::borrow(Py_None), false, 0, 0, sent.id);
    } else if (action == w::SqlAction::kNextResult) {
      if (st == nullptr || st->state == State::kIdle) return Ref::steal(Py_BuildValue("(i)", 3));
    } else if (action == w::SqlAction::kFetch) {
      if (st == nullptr || st->state != State::kRows) return error_tuple(1210, "HY000", "the result has no rows to fetch");
    } else if (st == nullptr || !st->prepared) {
      return error_tuple(1243, "HY000", "Unknown prepared statement handler given to EXECUTE");
    }
  }

  // The message. Its fields are decided here; the values are the codec's (encode_values).
  std::array<std::byte, w::kSqlCloseSize> bare{};
  std::span<std::byte> out;
  uint8_t wire_flags = 0;
  if (with_message) {
    const size_t size = size_t(PyByteArray_GET_SIZE(message));
    const auto *head = reinterpret_cast<const std::byte *>(PyByteArray_AS_STRING(message));
    uint32_t text_len = 0;
    if (size >= 17) std::memcpy(&text_len, head + 9, 4);
    const bool has_text = opens, has_values = action != w::SqlAction::kPrepare;
    if (size < 17 || text_len > size - 17 || (!has_text && text_len != 0) ||
        (!has_values && (count != 0 || size != size_t{text_len} + 17))) {
      PyErr_SetString(PyExc_ValueError, "sql: a message that does not fit its action");
      throw PyErrorSet{};
    }
    if (size + 9 > w::kMaxWorkerMessage) return Ref::borrow(Py_None);
    if (opens) {
      sent.to_client = (flags & w::kPassResultToClient) != 0;
      wire_flags = sent.to_client ? uint8_t{w::kPassResultToClient} : uint8_t{0};
    }
  }
  if (action == w::SqlAction::kExecuteMany) sent.rows = width != 0 ? count / width : 0;

  const HostCall waiting;
  if (opens) sent.id = ++g_statement_ids;
  if (with_message) {
    const Py_ssize_t size = PyByteArray_GET_SIZE(message);
    if (PyByteArray_Resize(message, size + 9) != 0) throw PyErrorSet{};
    out = {reinterpret_cast<std::byte *>(PyByteArray_AS_STRING(message)), size_t(size) + 9};
    uint32_t text_len;
    std::memcpy(&text_len, out.data() + 9, 4);
    const uint32_t values = count, max_rows = 0, row_width = action == w::SqlAction::kExecuteMany ? width : 0;
    std::memcpy(out.data() + 13 + text_len, &values, 4);
    out[size_t(size)] = std::byte{wire_flags};
    std::memcpy(out.data() + size + 1, &max_rows, 4);
    std::memcpy(out.data() + size + 5, &row_width, 4);
  } else {
    out = bare;
  }
  out[0] = std::byte{static_cast<uint8_t>(action)};
  std::memcpy(out.data() + 1, &sent.id, 8);
  Mailbox::send(uint32_t(w::Op::kSql), 0, q.depth, g_seq, out);
  Ref assembled;
  const Message m = sql_reply(assembled);
  if (m.discarded) {
    // §5.3: the worker cannot know the id's state: it closes the id and fails the statement.
    sql_close(q, sent.id);
    PyErr_SetString(PyExc_RuntimeError, "the result of the SQL statement is larger than this MLE worker can hold");
    throw PyErrorSet{};
  }
  Ref reply = sql_result(q, sent, m, assembled);
  // An interrupt that stopped a nested routine stops this one too: it could catch the error of
  // its statement and run on (§11.1). A CLOSE, which code that unwinds may send, is just done.
  if (Interrupts::interrupted() && action != w::SqlAction::kClose) {
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    throw PyErrorSet{};
  }
  return reply;
}

/** library(schema, name, known_version) -> (status, language, body, version, message), or None
    if the worker cannot hold the answer (§5.3): one LIBRARY message and its LIBRARY_RESULT
    (§9.10). schema and name are names: they are checked, never repaired (§7.1). */
Ref py_library(PyObject *args) {
  PyObject *schema, *name;
  unsigned long long known;
  if (!PyArg_ParseTuple(args, "UUK", &schema, &name, &known)) throw PyErrorSet{};
  if (host_unavailable("libraries can be imported")) throw PyErrorSet{};
  Request &q = *g_request;
  // After an interrupt an import fails, here without a message (§11.1).
  if (Interrupts::interrupted()) {
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    throw PyErrorSet{};
  }
  std::array<std::byte, 16 + 2 * w::kMaxLibraryName> msg;
  Writer wr(msg);
  for (PyObject *part : {schema, name}) {
    Py_ssize_t n = 0;
    const char *s = PyUnicode_AsUTF8AndSize(part, &n);
    if (s == nullptr) PyErr_Clear();
    if (s == nullptr || n < 1 || size_t(n) > w::kMaxLibraryName || std::memchr(s, 0, size_t(n)) != nullptr) {
      PyErr_Format(PyExc_ValueError,
                   "a library's schema and name must each be well-formed text of 1 to %zu bytes without U+0000",
                   w::kMaxLibraryName);
      throw PyErrorSet{};
    }
    (void)wr.str({s, size_t(n)});
  }
  (void)wr.put(static_cast<uint64_t>(known));
  const HostCall waiting;
  Mailbox::send(uint32_t(w::Op::kLibrary), 0, q.depth, g_seq, wr.written());
  Ref assembled;
  const Message m = receive(assembled);
  // An interrupt that stopped a nested routine, or came while the worker waited, stops this code.
  if (Interrupts::interrupted()) {
    PyErr_SetNone(PyExc_KeyboardInterrupt);
    throw PyErrorSet{};
  }
  if (m.discarded) return Ref::borrow(Py_None);
  Reader r(m.body);
  const auto status = r.get<uint8_t>();
  if (status > w::kMaxLibraryStatus) protocol_failure("an unknown LIBRARY_RESULT status");
  Ref language = str_of(r.str()), body = str_of(r.str());
  const uint64_t version = r.get<uint64_t>();
  Ref message = str_of(r.str());
  return Ref::steal(Py_BuildValue("(BNNKN)", status, language.release(), body.release(),
                                  static_cast<unsigned long long>(version), message.release()));
}

PyMethodDef kMethods[] = {
    {"serve", boundary<py_serve>, METH_VARARGS, "serve(handle, finish, convert, failed, vector_arg): the request loop"},
    {"ready", boundary<py_ready>, METH_VARARGS, "ready(info, runtime): post READY"},
    {"arena_size", boundary<py_arena_size>, METH_NOARGS, "the arena's size"},
    {"protocol_failure", boundary<py_protocol_failure>, METH_VARARGS, "protocol_failure(text): end the process"},
    {"register", boundary<py_register>, METH_VARARGS,
     "register(id, routine, fn, code, base, nparams, ret_tag, unsigned, vectors): a routine of the fast call path"},
    {"unregister", boundary<py_unregister>, METH_VARARGS, "unregister(id): drop a routine of the fast call path"},
    {"mark", boundary<py_mark>, METH_NOARGS, "the running call needs Python's tail"},
    {"printed", boundary<py_printed>, METH_NOARGS, "console output waits for a RESULT"},
    {"taint", boundary<py_taint>, METH_NOARGS, "the worker asks to be retired"},
    {"interrupt_due", boundary<py_interrupt_due>, METH_NOARGS, "whether SIGUSR1's handler raises KeyboardInterrupt"},
    {"interrupt_signal", boundary<py_interrupt_signal>, METH_VARARGS, "interrupt_signal(installed)"},
    {"guest_pause", boundary<py_guest_pause>, METH_NOARGS, "guest_pause() -> was: the worker's bookkeeping"},
    {"guest_unpause", boundary<py_guest_unpause>, METH_VARARGS, "guest_unpause(was)"},
    {"guest_exec", boundary<py_guest_exec>, METH_VARARGS, "guest_exec(code, globals, guest)"},
    {"trim", boundary<py_trim>, METH_NOARGS, "malloc_trim after RESET"},
    {"request", boundary<py_request>, METH_NOARGS, "the request being served, or None"},
    {"epoch", boundary<py_epoch>, METH_VARARGS, "epoch(library_epoch): of the PREPARE being served"},
    {"sql", boundary<py_sql>, METH_VARARGS, "sql(action, scope, id, message, count, flags, width) -> reply"},
    {"library", boundary<py_library>, METH_VARARGS, "library(schema, name, known_version) -> answer"},
    {"encode_values", boundary<py_encode_values>, METH_VARARGS, "encode_values(buffer, values, fallback) -> count"},
    {"setup", boundary<py_setup>, METH_VARARGS,
     "setup(decimal, json_loads, temporal, new_vector, demote, row_temporal, number, timestamp)"},
    {"decode", boundary<py_decode>, METH_VARARGS, "decode(body, pos, n[, vector]) -> (values, pos)"},
    {"decode_rows", boundary<py_decode_rows>, METH_VARARGS, "decode_rows(body, pos, nrows, ncols, row_type[, rules])"},
    {"decode_columns", boundary<py_decode_columns>, METH_VARARGS, "decode_columns(body, pos, nrows, collectors[, rules])"},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_mlewire", "The MLE CPython worker's mailbox, requests and codec.", -1,
                       kMethods, nullptr, nullptr, nullptr, nullptr};

}  // namespace

PyObject *init_mlewire() { return PyModule_Create(&kModule); }

}  // namespace mle::cpython
