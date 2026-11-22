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

#ifndef COMPONENTS_MLE_COMPONENT_ACCOUNTS_HPP
#define COMPONENTS_MLE_COMPONENT_ACCOUNTS_HPP

/**
  @file component/accounts.hpp

  Accounts in workers' principals, and the accounts that change (protocol
  §1, Principal; Appendix A, "Accounts that change"). An account here is one
  incarnation of it: no worker serves two incarnations of one account.

  * DROP USER and RENAME USER name the account they drop or rename away
    (the server's authentication events): every worker whose principal
    names it is retired.
  * CREATE USER, CREATE ROLE and the new name of a RENAME USER are not
    named. A worker records when it starts whether every account of its
    principal existed then; a creation retires only the workers that
    recorded one that did not (an owner that does not exist, a session that
    outlived its dropped account). Such a worker is never kept for another
    session either.

  Retirement is decided before the statement that changed the accounts
  reports success: the change is recorded here then, and a worker that is
  retired by it is never used again (a session's worker is ended before its
  next top-level request, a kept one at once).

  No MySQL dependencies: unit-tested on its own.
*/

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace mle_component {

/** An account as 'user'@'host' (quotes doubled; the host in lower case):
    the key of workers. */
std::string account_key(std::string_view user, std::string_view host);

/** In a worker's principal, between the account whose rights apply and the
    other account that owns the code (a routine of another definer that runs
    with the invoker's rights). */
inline constexpr std::string_view kCodeOf = " running code of ";

/** Whether the principal is the account's: runs for it or runs its code. */
bool principal_has_account(std::string_view principal,
                           std::string_view account);

/**
  The changes of accounts the host learned, numbered by one counter that
  only grows. Not thread-safe: the engine holds its mutex around it.
*/
class AccountLedger {
 public:
  /** DROP USER / RENAME USER (away from the name) of account
      (account_key()). Returns the new count. */
  uint64_t forget(const std::string &account);
  /** An account may have been created (CREATE USER, CREATE ROLE, RENAME
      USER). Returns the new count. */
  uint64_t created();
  /** The count now. */
  uint64_t count() const { return m_count; }

  /** Whether a worker of principal, whose state was known at count since,
      must be retired: an account of its principal was dropped or renamed
      after since; or, when unknown_at_start (it recorded at its start an
      account of its principal that did not exist), an account may have
      been created after since. */
  bool retire(std::string_view principal, uint64_t since,
              bool unknown_at_start) const;

  /** A session begins (its mark: the count now) or ends. What was
      forgotten before every session's mark is pruned: nothing older is
      asked about. */
  uint64_t begin_session();
  void end_session(uint64_t mark);

 private:
  void prune();

  uint64_t m_count = 0;
  /** m_count at the last creation. */
  uint64_t m_created_at = 0;
  /** m_count at each account's last DROP or RENAME. */
  std::map<std::string, uint64_t, std::less<>> m_forgotten_at;
  std::multiset<uint64_t> m_marks;
};

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_ACCOUNTS_HPP
