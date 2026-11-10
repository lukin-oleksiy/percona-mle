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

// Fuzz tests of the MLE wire protocol decoders: the worker is untrusted, so
// random and mutated message bodies must never crash the host or read out of
// bounds (run under ASAN/UBSAN to check the latter). Whatever a decoder
// accepts must re-encode to the same bytes.
//
// MLE_FUZZ_ITERATIONS (default 20000 per test) and MLE_FUZZ_SEED (default
// fixed) control the run; a failure prints the seed and the iteration.

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "mle/codec.h"
#include "mle/protocol.h"

namespace mle_codec_fuzz_unittest {

using namespace mle;

uint64_t env_or(const char *name, uint64_t def) {
  const char *v = std::getenv(name);
  return v != nullptr ? std::strtoull(v, nullptr, 10) : def;
}

const uint64_t kIterations = env_or("MLE_FUZZ_ITERATIONS", 20000);
const uint64_t kSeed = env_or("MLE_FUZZ_SEED", 20261110);

// Decodes body as every message type. For each accepted decode, encoding
// the result gives back body exactly (the encoding is canonical).
template <class M>
void check_one(std::string_view body) {
  M m;
  std::string error;
  if (!decode(body, &m, &error)) {
    ASSERT_FALSE(error.empty());
    return;
  }
  std::string again;
  encode(m, &again);
  ASSERT_EQ(again, body);
}

void check_all(std::string_view body) {
  check_one<ReadyMsg>(body);
  check_one<PrepareMsg>(body);
  check_one<CallMsg>(body);
  check_one<ResultMsg>(body);
  check_one<SqlMsg>(body);
  check_one<SqlResultMsg>(body);
  check_one<FetchMsg>(body);
  check_one<DropMsg>(body);
}

std::vector<std::string> seed_messages() {
  std::vector<std::string> out;
  auto add = [&out](const auto &m) {
    std::string b;
    encode(m, &b);
    out.push_back(std::move(b));
  };
  Temporal t;
  t.year = 2026, t.month = 11, t.day = 10, t.hour = 12, t.minute = 1;
  CallMsg call;
  call.routine_id = 3;
  call.args = std::vector<Value>{Value::null(),
                                 Value::i64(-1),
                                 Value::u64(2),
                                 Value::f64(0.5),
                                 Value::f32(1.5f),
                                 Value::decimal("-1.25"),
                                 Value::string("caf\xc3\xa9"),
                                 Value::bytes(std::string("\0\1\2", 3)),
                                 Value::temporal(Tag::kDatetime, t),
                                 Value::json("[1]"),
                                 Value::vector({1, 2, 3, 4, 5})};
  add(call);
  ResultMsg result;
  result.ret = Value::string("ok");
  result.out_params.emplace_back(0, Value::i64(1));
  result.console_out = "out";
  result.error = Error{"python", "boom", {{"f", "r.py", 1, 2}}, 1146, "42S02"};
  add(result);
  SqlMsg sql;
  sql.statement = "SELECT ?";
  sql.params = std::vector<Value>{Value::vector({0.5f})};
  add(sql);
  SqlResultMsg rows;
  rows.kind = SqlResultKind::kRows;
  rows.columns = {{"a", "t", {}}, {"b", "t", {}}};
  rows.rows = std::vector<Value>{Value::i64(1), Value::null(), Value::i64(2),
                                 Value::string("x")};
  rows.warnings = {{1, 2, "w"}};
  add(rows);
  PrepareMsg prepare;
  prepare.language = "js";
  prepare.entry = "f";
  prepare.source = "function f(a) { return a; }";
  prepare.params = {{"a", ParamMode::kInOut, {}}};
  prepare.returns = ParamType{};
  add(prepare);
  ReadyMsg ready;
  ready.languages = {"java", "js"};
  add(ready);
  add(FetchMsg{10});
  add(DropMsg{11});
  return out;
}

TEST(MleCodecFuzz, RandomBytes) {
  std::mt19937_64 rng(kSeed);
  std::string body;
  for (uint64_t i = 0; i < kIterations; i++) {
    body.resize(rng() % 128);
    for (char &c : body) c = char(rng());
    // Bias towards small counts and known tags, so decoding gets deeper.
    if (!body.empty() && rng() % 2 == 0) body[0] = char(rng() % 14);
    SCOPED_TRACE("seed " + std::to_string(kSeed) + " iteration " +
                 std::to_string(i));
    check_all(body);
    if (HasFatalFailure()) return;
  }
}

TEST(MleCodecFuzz, MutatedMessages) {
  std::mt19937_64 rng(kSeed + 1);
  const std::vector<std::string> seeds = seed_messages();
  for (uint64_t i = 0; i < kIterations; i++) {
    std::string body = seeds[rng() % seeds.size()];
    const int mutations = 1 + int(rng() % 4);
    for (int k = 0; k < mutations && !body.empty(); k++) {
      const size_t at = rng() % body.size();
      switch (rng() % 6) {
        case 0:  // flip a bit
          body[at] = char(body[at] ^ (1 << (rng() % 8)));
          break;
        case 1:  // replace a byte
          body[at] = char(rng());
          break;
        case 2:  // interesting byte values (counts, lengths)
          body[at] = "\x00\x01\x7f\x80\xff"[rng() % 5];
          break;
        case 3:  // truncate
          body.resize(at);
          break;
        case 4:  // insert
          body.insert(at, 1, char(rng()));
          break;
        case 5:  // erase
          body.erase(at, 1 + rng() % 4);
          break;
      }
    }
    SCOPED_TRACE("seed " + std::to_string(kSeed) + " iteration " +
                 std::to_string(i));
    check_all(body);
    if (HasFatalFailure()) return;
  }
}

// Random chunk sequences: the assembler either completes with exactly the
// announced bytes or fails, and never holds more than the announced total.
TEST(MleCodecFuzz, Assembler) {
  std::mt19937_64 rng(kSeed + 2);
  const uint32_t arena = 256;
  const std::string data(arena, 'z');
  for (uint64_t i = 0; i < kIterations; i++) {
    Assembler a(arena, 4096);
    wire::MessageHeader h{};
    h.op = uint32_t(wire::Op::kResult);
    h.total_len = rng() % 5000;
    for (int k = 0; k < 40; k++) {
      if (rng() % 8 == 0) h.call_seq = rng() % 2;
      if (rng() % 16 == 0) h.total_len = rng() % 5000;
      h.len = uint32_t(rng() % (arena + 20));
      h.flags = rng() % 4 == 0 ? 0U : uint32_t{wire::kMore};
      const Assembler::Step step = a.add(h, data.data());
      ASSERT_LE(a.body().size(), a.header().total_len);
      if (step == Assembler::Step::kDone) {
        ASSERT_EQ(a.body().size(), a.header().total_len);
        break;
      }
      if (step == Assembler::Step::kError) {
        ASSERT_FALSE(a.error().empty());
        break;
      }
    }
  }
}

}  // namespace mle_codec_fuzz_unittest
