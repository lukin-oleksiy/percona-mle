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
  @file worker/cpython/mailbox.hpp

  The CPython worker on the shared mailbox (worker/common/mailbox.hpp): the futex sleep without the
  GIL, and KILL as KeyboardInterrupt raised in the thread that serves requests.
*/

#ifndef MLE_CPYTHON_MAILBOX_HPP
#define MLE_CPYTHON_MAILBOX_HPP

#include <cstdint>

#include "common/mailbox.hpp"

namespace mle::cpython {

using common::Interrupts;
using common::Mailbox;
using common::Message;

/** Mailbox::open with the CPython hooks; in the thread that serves requests, with the GIL. */
uint32_t open_mailbox(int spin);

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_MAILBOX_HPP
