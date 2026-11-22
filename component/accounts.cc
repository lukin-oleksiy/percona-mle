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

#include "accounts.hpp"

#include <algorithm>

namespace mle_component {

std::string account_key(std::string_view user, std::string_view host) {
  // MySQL's quoting: unambiguous ('a@b'@'c' is not 'a'@'b@c').
  std::string key;
  key.reserve(user.size() + host.size() + 8);
  key += '\'';
  for (char c : user) {
    key += c;
    if (c == '\'') key += c;
  }
  key += "'@'";
  // Host names compare without case (a definer's host may be written in
  // another case than the account's).
  for (char c : host) {
    key += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (c == '\'') key += c;
  }
  key += '\'';
  return key;
}

bool principal_has_account(std::string_view principal,
                           std::string_view account) {
  if (principal == account) return true;
  const size_t n = account.size() + kCodeOf.size();
  return principal.size() > n &&
         ((principal.starts_with(account) &&
           principal.substr(account.size(), kCodeOf.size()) == kCodeOf) ||
          (principal.ends_with(account) &&
           principal.substr(principal.size() - n, kCodeOf.size()) == kCodeOf));
}

uint64_t AccountLedger::forget(const std::string &account) {
  m_forgotten_at[account] = ++m_count;
  prune();
  return m_count;
}

uint64_t AccountLedger::created() {
  m_created_at = ++m_count;
  return m_count;
}

bool AccountLedger::retire(std::string_view principal, uint64_t since,
                           bool unknown_at_start) const {
  if (unknown_at_start && m_created_at > since) return true;
  // Either account of the principal: whose rights the code runs with, whose
  // code it is.
  for (const auto &[account, at] : m_forgotten_at)
    if (at > since && principal_has_account(principal, account)) return true;
  return false;
}

uint64_t AccountLedger::begin_session() {
  m_marks.insert(m_count);
  return m_count;
}

void AccountLedger::end_session(uint64_t mark) {
  if (const auto it = m_marks.find(mark); it != m_marks.end()) m_marks.erase(it);
  prune();
}

void AccountLedger::prune() {
  // Every question is asked with a count at or after the oldest mark.
  const uint64_t oldest = m_marks.empty() ? m_count : *m_marks.begin();
  std::erase_if(m_forgotten_at,
                [oldest](const auto &entry) { return entry.second <= oldest; });
}

}  // namespace mle_component
