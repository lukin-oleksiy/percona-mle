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
  GIL, and an interrupt (§11.1) as KeyboardInterrupt raised in the thread that serves requests,
  only while guest code of the interrupted request runs.
*/

#ifndef MLE_CPYTHON_MAILBOX_HPP
#define MLE_CPYTHON_MAILBOX_HPP

#include <cstdint>

#include "common/mailbox.hpp"

namespace mle::cpython {

using common::Interrupts;
using common::Mailbox;
using common::Message;

/** Mailbox::open with the CPython hooks (§2 steps 3 and 4), before Python starts. */
uint32_t open_mailbox(int spin);

/** Python runs: the calling thread is the one that serves requests. With the GIL. */
void serving_thread();
/** Whether the caller is the thread that serves requests. */
bool on_serving_thread();

/** Guest code of the request being served starts or goes on: an interrupt delivered to the
    top-level request raises KeyboardInterrupt in it from here on. With the GIL. */
void guest_enter();
/** Guest code goes on after a wait for the host. An interrupt that was delivered meanwhile is
    raised by the caller itself, or by the helper thread a moment later: what the guest code runs
    while it unwinds gets that moment. With the GIL. */
void guest_resume();
/** Guest code stops (it returned, or it waits for the host): a KeyboardInterrupt that was set and
    not raised yet is dropped, and none is set until guest_enter(). With the GIL. */
void guest_leave();

}  // namespace mle::cpython

#endif  // MLE_CPYTHON_MAILBOX_HPP
