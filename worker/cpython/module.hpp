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

  _mlewire, the built-in module the worker's Python code (python/mle_cpython) uses: the request
  loop with the fast call path, the requests being served and their SQL statements (§10.1), SQL
  and LIBRARY round trips, READY and RESULT, and the value codec.
*/

#ifndef MLE_CPYTHON_MODULE_HPP
#define MLE_CPYTHON_MODULE_HPP

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>

namespace mle::cpython {

/** What the command line sets (main.cc). */
struct Config {
  /** The largest host message the worker holds, in bytes; a larger one is discarded and failed
      where it belongs (§5.3). 0: whatever memory can be allocated. */
  uint64_t message_max = 0;
  /** Kept statements (§10.1) per routine, at most; 0: statements are never kept. */
  uint32_t kept_statements = 32;
  /** The arena's size (Mailbox::open). */
  uint32_t arena_size = 0;
};
Config &config();

/** For PyImport_AppendInittab("_mlewire", ...). */
PyObject *init_mlewire();

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_MODULE_HPP
