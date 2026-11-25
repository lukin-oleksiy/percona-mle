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
#include <signal.h>
#include <unistd.h>

namespace mle::cpython {

namespace {

/** The thread that serves requests (Python's main thread): where an interrupt raises. */
unsigned long g_main_ident = 0;
/** The serving thread's state while it sleeps on the futex without the GIL. */
PyThreadState *g_saved = nullptr;
/** Guest code of the request being served may be running. Read and written with the GIL. */
bool g_guest = false;
/** The serving thread, for pthread_kill. */
pthread_t g_main_thread;
/** A callable: whether the worker's SIGUSR1 handler is installed (interrupt_signal), or null. */
PyObject *g_signal_installed = nullptr;

/** The serving thread is woken from a blocking call by SIGUSR1, if the worker's handler for it is
    still installed: guest code that replaced it (or set SIG_DFL, which would end the process) is
    not signalled, and KILL waits for the grace period as before (MVP review D11). The handler
    raises only while interrupt_due(): a signal that lands in the worker's own code or after the
    request returns nothing. With the GIL. */
void wake_main() {
  if (g_signal_installed == nullptr) return;
  PyObject *r = PyObject_CallNoArgs(g_signal_installed);
  const bool installed = r == Py_True;
  Py_XDECREF(r);
  if (r == nullptr) PyErr_Clear();
  if (installed) pthread_kill(g_main_thread, SIGUSR1);
}

void before_sleep() { g_saved = PyEval_SaveThread(); }
void after_sleep() { PyEval_RestoreThread(g_saved); }

/** A helper thread per delivered interrupt. With the GIL (a Python loop gives it up every few
    milliseconds), and only while the same request still runs and its guest code may be running,
    it sets KeyboardInterrupt for the serving thread, which raises at its next bytecode; and again
    every 10 ms, so that guest code that catches the exception does not run on, and what it runs
    while it unwinds is bounded (§11.1). The GIL
    orders it with guest_leave() and with the end of the request, which is fixed with the GIL
    held: an interrupt never reaches the worker's own code or a later request. A thread of its
    own, so that the control thread still reads end of file while the GIL is busy in native
    code. */
void *interrupter(void *arg) {
  const auto seq = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(arg));
  while (true) {
    const PyGILState_STATE gil = PyGILState_Ensure();
    const bool running = Interrupts::running(seq);
    if (running && g_guest) {
      PyThreadState_SetAsyncExc(g_main_ident, PyExc_KeyboardInterrupt);
      wake_main();
    }
    PyGILState_Release(gil);
    if (!running) return nullptr;
    usleep(10000);
  }
}

/** EngineHooks::interrupt: hand over to a helper thread. */
void interrupt(uint64_t seq) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 256 << 10);
  pthread_t t;
  // If no thread can be made, guest_enter() still raises at the next start of guest code, and
  // the host ends the worker after the grace period.
  pthread_create(&t, &attr, interrupter, reinterpret_cast<void *>(static_cast<uintptr_t>(seq)));
  pthread_attr_destroy(&attr);
}

/** EngineHooks::interrupted_end: a KeyboardInterrupt set but not raised yet is dropped. */
void interrupted_end() { PyThreadState_SetAsyncExc(g_main_ident, nullptr); }

}  // namespace

uint32_t open_mailbox(int spin) {
  common::EngineHooks hooks;
  hooks.before_sleep = before_sleep;
  hooks.after_sleep = after_sleep;
  hooks.interrupt = interrupt;
  hooks.interrupted_end = interrupted_end;
  return Mailbox::open(spin, hooks, "mle_cpython_worker");
}

void serving_thread() {
  g_main_ident = PyThread_get_thread_ident();
  g_main_thread = pthread_self();
}

bool guest_running() { return g_guest; }

bool interrupt_due() { return g_guest && Interrupts::interrupted(); }

void interrupt_signal(PyObject *installed) { Py_XSETREF(g_signal_installed, Py_XNewRef(installed)); }

bool on_serving_thread() { return PyThread_get_thread_ident() == g_main_ident; }

void guest_enter() {
  g_guest = true;
  if (Interrupts::interrupted()) PyThreadState_SetAsyncExc(g_main_ident, PyExc_KeyboardInterrupt);
}

void guest_resume() { g_guest = true; }

void guest_leave() {
  g_guest = false;
  if (Interrupts::interrupted()) PyThreadState_SetAsyncExc(g_main_ident, nullptr);
}

}  // namespace mle::cpython
