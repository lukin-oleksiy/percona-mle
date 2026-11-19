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
  @file worker/cpython/module.hpp

  _mlewire, the built-in module the worker's Python code (python/mle_cpython) uses: the mailbox,
  interrupts, the codec and the fast RESULT path.
*/

#ifndef MLE_CPYTHON_MODULE_HPP
#define MLE_CPYTHON_MODULE_HPP

#define PY_SSIZE_T_CLEAN
#include <Python.h>

namespace mle::cpython {

/** For PyImport_AppendInittab("_mlewire", ...). */
PyObject *init_mlewire();

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_MODULE_HPP
