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

// mle_proto_scripts: the message scripts of the scripted test worker (mle_test_worker proto) that
// the MySQL test test_worker_violations gives as the bodies of LANGUAGE TEST routines (exp design
// note 66, MVP review TEST-1). Prints one mysqltest line per script:
//   --let $<name> = call:<hex>
// The scripts are built with proto_script.h, as in worker_protocol-t.cc, so that MTR never writes
// message bytes by hand.

#include <cstdio>
#include <string>

#include "mle/codec.h"
#include "proto_script.h"

namespace {

using namespace mle;
using mle_test::Script;
using wire::Op;

uint32_t u(Op op) { return static_cast<uint32_t>(op); }

template <class M>
std::string bytes_of(const M &m) {
  std::string body;
  encode(m, &body);
  return body;
}

ResultMsg returning(Value v) {
  ResultMsg m;
  m.ret = std::move(v);
  return m;
}

void print(const char *name, const Script &s) { std::printf("--let $%s = call:%s\n", name, s.hex().c_str()); }

}  // namespace

int main() {
  // A worker that does what it should: a function that returns 7.
  print("script_ok", Script().result(returning(Value::i64(7))));
  // A RESULT with a status no worker may send (§9.4): a violation.
  print("script_bad_status", Script().post(u(Op::kResult), 9, 0, bytes_of(returning(Value::i64(7)))));
  // A value that breaks its domain (§7.2: a DATE of month 13): a violation.
  Temporal t;
  t.year = 2024;
  t.month = 13;
  t.day = 1;
  print("script_bad_domain", Script().result(returning(Value::temporal(wire::Tag::kDate, t))));
  // An SQL message at a depth that is not the request's (§8.1): a violation.
  SqlMsg sql;
  sql.action = wire::SqlAction::kExecute;
  sql.stmt_id = 1;
  sql.statement = "SELECT 1";
  print("script_wrong_depth", Script().post(u(Op::kSql), 0, 0, bytes_of(sql), mle_test::kAuto, mle_test::kAuto, 1));
  // A worker that exits in the middle of a call (§11.4): died.
  print("script_exits", Script().exit(7));
  // A worker that does not stop when interrupted (§11.1): killed after the grace period.
  print("script_sleeps", Script().sleep(60000));
  return 0;
}
