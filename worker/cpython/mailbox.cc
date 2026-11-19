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

#include "mailbox.hpp"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pthread.h>

namespace mle::cpython {

namespace {

/** The thread that runs requests (Python's main thread): where KILL raises. */
unsigned long g_main_ident = 0;
/** The serving thread's state while it sleeps on the futex without the GIL. */
PyThreadState *g_saved = nullptr;

void before_sleep() { g_saved = PyEval_SaveThread(); }
void after_sleep() { PyEval_RestoreThread(g_saved); }

/** A helper thread per INTERRUPT: takes the GIL (a Python loop gives it up every few milliseconds),
    and while the same request still runs (it cannot end meanwhile: the GIL is held here) raises
    KeyboardInterrupt in the main thread at its next bytecode. Neither PyErr_SetInterruptEx nor
    Py_AddPendingCall do that from another thread in CPython 3.12: they only mark the event, and a
    busy loop ran on until the host's SIGKILL. A thread of its own, so that the control thread
    still takes SHUTDOWN and EOF while the GIL is busy in native code. */
void *interrupter(void *arg) {
  const auto seq = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(arg));
  const PyGILState_STATE gil = PyGILState_Ensure();
  if (Interrupts::running(seq)) PyThreadState_SetAsyncExc(g_main_ident, PyExc_KeyboardInterrupt);
  PyGILState_Release(gil);
  return nullptr;
}

/** The control thread, with the interrupt lock held: hand over to a helper thread. */
void interrupt(uint64_t seq) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 256 << 10);
  pthread_t t;
  // If no thread can be made, the host's SIGKILL after the grace ends the request.
  pthread_create(&t, &attr, interrupter, reinterpret_cast<void *>(static_cast<uintptr_t>(seq)));
  pthread_attr_destroy(&attr);
}

/** A KeyboardInterrupt set but not raised yet (the request returned first) is dropped. */
void interrupted_end() { PyThreadState_SetAsyncExc(g_main_ident, nullptr); }

}  // namespace

uint32_t open_mailbox(int spin) {
  g_main_ident = PyThread_get_thread_ident();
  common::EngineHooks hooks;
  hooks.before_sleep = before_sleep;
  hooks.after_sleep = after_sleep;
  hooks.interrupt = interrupt;
  hooks.interrupted_end = interrupted_end;
  return Mailbox::open(spin, hooks, "mle_cpython_worker");
}

}  // namespace mle::cpython
