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

// The rule for accounts that change (docs/protocol.md §1, Principal;
// Appendix A, "Accounts that change"), as component_mle keeps it
// (component/accounts.hpp): a DROP or RENAME names the account and retires
// the workers whose principal names it; a creation is not named and retires
// only the workers that recorded at their start an account of their
// principal that did not exist then.

#include <gtest/gtest.h>

#include <string>

#include "accounts.hpp"

namespace mle_accounts_unittest {

using mle_component::account_key;
using mle_component::AccountLedger;
using mle_component::kCodeOf;
using mle_component::principal_has_account;

const std::string kAlice = account_key("alice", "%");
const std::string kBob = account_key("bob", "localhost");
const std::string kCarol = account_key("carol", "%");
/** Bob's invoker-rights routine of Alice: Bob's rights, Alice's code. */
const std::string kBobRunsAlice = kBob + std::string(kCodeOf) + kAlice;

TEST(MleAccounts, Keys) {
  EXPECT_EQ(account_key("a'b", "Host"), "'a''b'@'host'");
  // Unambiguous: the quote of a name is doubled.
  EXPECT_NE(account_key("a@b", "c"), account_key("a", "b@c"));
  EXPECT_TRUE(principal_has_account(kAlice, kAlice));
  EXPECT_TRUE(principal_has_account(kBobRunsAlice, kAlice));
  EXPECT_TRUE(principal_has_account(kBobRunsAlice, kBob));
  EXPECT_FALSE(principal_has_account(kBobRunsAlice, kCarol));
  // A name that only ends like the account is not the account.
  EXPECT_FALSE(principal_has_account(account_key("xalice", "%"), kAlice));
  EXPECT_FALSE(principal_has_account(kBob + "x" + std::string(kCodeOf) + kAlice,
                                     kBob));
}

TEST(MleAccounts, DropAndRenameRetireTheWorkersThatNameTheAccount) {
  AccountLedger l;
  const uint64_t mark = l.begin_session();
  const uint64_t started = l.count();
  l.forget(kAlice);
  // Whose rights or whose code: either account.
  EXPECT_TRUE(l.retire(kAlice, started, false));
  EXPECT_TRUE(l.retire(kBobRunsAlice, started, false));
  EXPECT_FALSE(l.retire(kBob, started, false));
  EXPECT_FALSE(l.retire(kCarol, started, true));
  // A worker started after the drop is not retired by it.
  EXPECT_FALSE(l.retire(kAlice, l.count(), false));
  l.end_session(mark);
}

TEST(MleAccounts, CreationRetiresOnlyWorkersThatRecordedAnUnknownAccount) {
  AccountLedger l;
  const uint64_t mark = l.begin_session();
  const uint64_t started = l.count();
  l.created();
  // The old rule retired every worker of two accounts; the revised one only
  // those that recorded at their start an account that did not exist: an
  // owner that did not exist (an orphan definer) ...
  EXPECT_FALSE(l.retire(kBobRunsAlice, started, false));
  EXPECT_TRUE(l.retire(kBobRunsAlice, started, true));
  // ... or a session that outlived its account.
  EXPECT_FALSE(l.retire(kBob, started, false));
  EXPECT_TRUE(l.retire(kBob, started, true));
  // A worker that started after the creation is not retired by it.
  EXPECT_FALSE(l.retire(kBob, l.count(), true));
  l.end_session(mark);
}

// A session that outlived its account: the account the session knew was
// dropped after the session began, so a worker it starts for it records an
// account that does not exist, even when one of that name exists again;
// then a creation retires that worker.
TEST(MleAccounts, ASessionOutlivesItsAccount) {
  AccountLedger l;
  const uint64_t old_session = l.begin_session();
  l.forget(kBob);   // DROP USER bob
  l.created();      // CREATE USER bob: another incarnation
  const uint64_t new_session = l.begin_session();
  // Asked with the account as the principal and the session's mark: the
  // old session's bob is gone, the new session's is not.
  EXPECT_TRUE(l.retire(kBob, old_session, false));
  EXPECT_FALSE(l.retire(kBob, new_session, false));
  // The old session's worker for bob started now recorded it as unknown.
  const uint64_t started = l.count();
  EXPECT_FALSE(l.retire(kBob, started, true));
  l.created();
  EXPECT_TRUE(l.retire(kBob, started, true));
  // A worker of the new session's bob (all accounts known) stays.
  EXPECT_FALSE(l.retire(kBob, started, false));
  l.end_session(old_session);
  l.end_session(new_session);
}

TEST(MleAccounts, ForgottenAccountsArePrunedWhenNoSessionIsOlder) {
  AccountLedger l;
  const uint64_t a = l.begin_session();
  l.forget(kAlice);
  const uint64_t b = l.begin_session();
  l.forget(kCarol);
  EXPECT_TRUE(l.retire(kAlice, a, false));
  l.end_session(a);
  // Session b began after Alice was dropped: that drop is not its concern.
  EXPECT_FALSE(l.retire(kAlice, b, false));
  EXPECT_TRUE(l.retire(kCarol, b, false));
  l.end_session(b);
  const uint64_t c = l.begin_session();
  EXPECT_FALSE(l.retire(kCarol, c, false));
  l.end_session(c);
}

}  // namespace mle_accounts_unittest
