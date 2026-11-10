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

// Routine bodies as in CREATE ... AS $$ body $$ (P4-W3): Oracle's
// JavaScript form (named parameters, OUT/INOUT, per-call globalThis, await,
// Oracle's conversions and options, errors and positions), and the Python
// and Java body forms. Through mle::Worker and the JVM worker.

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "jvm_test_util.h"

namespace mle_body_forms_unittest {

using namespace mle;
using namespace mle_test;
using wire::ParamMode;

struct P {
  std::string name;
  ParamMode mode;
  ParamType type;
};

ParamType ty(Tag tag, const std::string &name, bool is_unsigned = false) {
  ParamType t;
  t.tag = tag;
  t.type_name = name;
  t.is_unsigned = is_unsigned;
  return t;
}

const ParamType kInt = ty(Tag::kI64, "INT");
const ParamType kBigint = ty(Tag::kI64, "BIGINT");
const ParamType kVarchar = ty(Tag::kString, "VARCHAR");

Value dt(Tag tag, uint16_t y, uint8_t mo, uint8_t d, uint32_t h, uint8_t mi,
         uint8_t s) {
  Temporal t;
  t.year = y;
  t.month = mo;
  t.day = d;
  t.hour = h;
  t.minute = mi;
  t.second = s;
  return Value::temporal(tag, t);
}

class MleBodyForms : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.classes = {jvm_class("body", "java,js,python")};
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
    auto w = s_launcher->acquire("body", &err);
    ASSERT_NE(w, nullptr) << err;
    s_host = new TestHost(std::move(w));
    ReadyMsg r;
    ASSERT_TRUE(s_host->wait_ready(&r, &err)) << err;
  }
  static void TearDownTestSuite() {
    delete s_host;
    delete s_launcher;
  }
  void TearDown() override { set_options({}); }

  static uint64_t next_id() { return ++s_id; }

  /** PREPAREs a body; returns the routine id, or 0 with *error. */
  static uint64_t prepare(const std::string &lang, const std::string &name,
                          const std::string &body, std::vector<P> params,
                          std::optional<ParamType> ret,
                          std::string *error = nullptr) {
    PrepareMsg p;
    p.routine_id = next_id();
    p.language = lang;
    p.name = name;
    p.source = body;
    for (auto &x : params) p.params.push_back({x.name, x.mode, x.type});
    p.returns = ret;
    ResultMsg res;
    const uint32_t s = s_host->prepare(p, &res);
    if (s != 0) {
      if (error) *error = error_of(res);
      return 0;
    }
    return p.routine_id;
  }

  static ResultMsg call(uint64_t id, const std::vector<Value> &args,
                        uint32_t *status = nullptr) {
    ResultMsg res;
    const uint32_t s = s_host->call(id, args, &res);
    if (status) *status = s;
    return res;
  }

  /** The return value of a call that must succeed. */
  static Value ret(uint64_t id, const std::vector<Value> &args) {
    uint32_t s;
    ResultMsg r = call(id, args, &s);
    EXPECT_EQ(s, 0u) << error_of(r);
    return r.ret ? *r.ret : Value();
  }

  static void set_options(const OptionsMsg &o) {
    std::string err;
    ASSERT_EQ(s_host->worker.options(o, &err), Status::kOk) << err;
  }

  /** A one-expression JS function returning type t: return (expr). */
  static uint64_t js_expr(const std::string &expr, const ParamType &t) {
    std::string err;
    const uint64_t id = prepare("js", "e", "return (" + expr + ");", {}, t, &err);
    EXPECT_NE(id, 0u) << expr << ": " << err;
    return id;
  }

  static inline Launcher *s_launcher = nullptr;
  static inline TestHost *s_host = nullptr;
  static inline uint64_t s_id = 0;
};

// ---------------------------------------------------------------- JavaScript

TEST_F(MleBodyForms, JsFunctionBody) {
  const uint64_t id = prepare("js", "add", "return a + b",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kIn, kInt}},
                              kBigint);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {Value::i64(2), Value::i64(40)}), Value::i64(42));
}

TEST_F(MleBodyForms, JsProcedureOutAndInout) {
  std::string err;
  const uint64_t id = prepare("js", "proc",
                              "if (b !== null) throw new Error('OUT must start as null');\n"
                              "b = a * 2;\n"
                              "c = c + '!';\n"
                              "if (a > 0) return;\n"
                              "c = 'not reached';",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kOut, kInt},
                               {"c", ParamMode::kInOut, kVarchar}},
                              std::nullopt, &err);
  ASSERT_NE(id, 0u) << err;
  uint32_t s;
  ResultMsg r = call(id, {Value::i64(21), Value::i64(7), Value::string("hi")}, &s);
  ASSERT_EQ(s, 0u) << error_of(r);
  ASSERT_EQ(r.out_params.size(), 2u);
  EXPECT_EQ(r.out_params[0].first, 1u);
  EXPECT_EQ(r.out_params[0].second, Value::i64(42));
  EXPECT_EQ(r.out_params[1].first, 2u);
  EXPECT_EQ(r.out_params[1].second, Value::string("hi!"));
}

TEST_F(MleBodyForms, JsGlobalThisIsPerCall) {
  const uint64_t id = prepare(
      "js", "g",
      "globalThis.n = (globalThis.n ?? 0) + 1;\n"
      "return globalThis.n + ':' + typeof globalThis.Math + ':' + typeof JSON;",
      {}, kVarchar);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {}), Value::string("1:object:object"));
  EXPECT_EQ(ret(id, {}), Value::string("1:object:object"));
  // Nothing leaked into the real global object either.
  const uint64_t other = js_expr("typeof n", kVarchar);
  EXPECT_EQ(ret(other, {}), Value::string("undefined"));
}

TEST_F(MleBodyForms, JsStrictMode) {
  const uint64_t id = prepare("js", "s", "undeclared = 5;\nreturn 1;", {}, kInt);
  ASSERT_NE(id, 0u);
  uint32_t s;
  ResultMsg r = call(id, {}, &s);
  EXPECT_EQ(s, uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(r).find("ReferenceError"), std::string::npos) << error_of(r);
}

TEST_F(MleBodyForms, JsAwait) {
  const uint64_t f = prepare("js", "aw",
                             "const v = await Promise.resolve(a);\nreturn v + 1;",
                             {{"a", ParamMode::kIn, kInt}}, kInt);
  ASSERT_NE(f, 0u);
  EXPECT_EQ(ret(f, {Value::i64(41)}), Value::i64(42));

  const uint64_t p = prepare("js", "awp", "b = await Promise.resolve(a * 3);",
                             {{"a", ParamMode::kIn, kInt},
                              {"b", ParamMode::kOut, kInt}},
                             std::nullopt);
  ASSERT_NE(p, 0u);
  uint32_t s;
  ResultMsg r = call(p, {Value::i64(5), Value::null()}, &s);
  ASSERT_EQ(s, 0u) << error_of(r);
  ASSERT_EQ(r.out_params.size(), 1u);
  EXPECT_EQ(r.out_params[0].second, Value::i64(15));

  const uint64_t bad = prepare("js", "awe", "await null;\nthrow new Error('boom');",
                               {}, kInt);
  ASSERT_NE(bad, 0u);
  r = call(bad, {}, &s);
  EXPECT_EQ(s, uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(r), "Error: boom");
}

TEST_F(MleBodyForms, JsIntegerTypeOption) {
  const uint64_t id = prepare("js", "it", "return typeof a + ':' + String(a);",
                              {{"a", ParamMode::kIn, kBigint}}, kVarchar);
  ASSERT_NE(id, 0u);
  const Value small = Value::i64(5);
  const Value big = Value::i64(int64_t{1} << 60);
  // Oracle's default UNSAFE_STRING: Number when safe, else String.
  EXPECT_EQ(ret(id, {small}), Value::string("number:5"));
  EXPECT_EQ(ret(id, {big}), Value::string("string:1152921504606846976"));
  OptionsMsg o;
  o.integer_type = wire::IntegerType::kUnsafeBigint;
  set_options(o);
  EXPECT_EQ(ret(id, {small}), Value::string("number:5"));
  EXPECT_EQ(ret(id, {big}), Value::string("bigint:1152921504606846976"));
  o.integer_type = wire::IntegerType::kBigint;
  set_options(o);
  EXPECT_EQ(ret(id, {small}), Value::string("bigint:5"));
  o.integer_type = wire::IntegerType::kString;
  set_options(o);
  EXPECT_EQ(ret(id, {small}), Value::string("string:5"));
  // BIGINT UNSIGNED beyond 2^63.
  const uint64_t u = prepare("js", "iu", "return typeof a + ':' + String(a);",
                             {{"a", ParamMode::kIn, ty(Tag::kU64, "BIGINT", true)}},
                             kVarchar);
  set_options({});
  EXPECT_EQ(ret(u, {Value::u64(UINT64_MAX)}),
            Value::string("string:18446744073709551615"));
}

TEST_F(MleBodyForms, JsDecimalTypeOption) {
  const uint64_t id = prepare("js", "dt", "return typeof a + ':' + String(a);",
                              {{"a", ParamMode::kIn, ty(Tag::kDecimal, "DECIMAL")}},
                              kVarchar);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {Value::decimal("1.50")}), Value::string("string:1.50"));
  OptionsMsg o;
  o.decimal_type = wire::DecimalType::kNumber;
  set_options(o);
  EXPECT_EQ(ret(id, {Value::decimal("1.50")}), Value::string("number:1.5"));
}

TEST_F(MleBodyForms, JsSetEnumYearBit) {
  const uint64_t id = prepare(
      "js", "st",
      "return [a instanceof Set, a.size, a.has('c'), typeof e, typeof y, typeof b].join(':');",
      {{"a", ParamMode::kIn, ty(Tag::kString, "SET")},
       {"e", ParamMode::kIn, ty(Tag::kString, "ENUM")},
       {"y", ParamMode::kIn, ty(Tag::kI64, "YEAR")},
       {"b", ParamMode::kIn, ty(Tag::kU64, "BIT", true)}},
      kVarchar);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {Value::string("a,c"), Value::string("x"), Value::i64(2024),
                     Value::u64(5)}),
            Value::string("true:2:true:string:number:number"));
  const uint64_t empty = prepare("js", "se", "return a.size;",
                                 {{"a", ParamMode::kIn, ty(Tag::kString, "SET")}}, kInt);
  EXPECT_EQ(ret(empty, {Value::string("")}), Value::i64(0));
  EXPECT_EQ(ret(js_expr("new Set(['x', 'y'])", ty(Tag::kString, "SET")), {}),
            Value::string("x,y"));
}

TEST_F(MleBodyForms, JsDatesInTheSessionTimeZone) {
  OptionsMsg o;
  o.time_zone = "+02:00";
  set_options(o);
  const uint64_t id = prepare("js", "d", "return a.toISOString();",
                              {{"a", ParamMode::kIn, ty(Tag::kDatetime, "DATETIME")}},
                              kVarchar);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {dt(Tag::kDatetime, 2024, 1, 2, 3, 4, 5)}),
            Value::string("2024-01-02T01:04:05.000Z"));
  EXPECT_EQ(ret(js_expr("new Date(Date.UTC(2024, 0, 2, 1, 4, 5))",
                        ty(Tag::kDatetime, "DATETIME")),
                {}),
            dt(Tag::kDatetime, 2024, 1, 2, 3, 4, 5));
  EXPECT_EQ(ret(js_expr("new Date(Date.UTC(2024, 0, 2, 23, 0, 0))",
                        ty(Tag::kDate, "DATE")),
                {}),
            dt(Tag::kDate, 2024, 1, 3, 0, 0, 0));
  o.time_zone = "Europe/Kyiv";
  set_options(o);
  o.time_zone = "Mars/Olympus";
  std::string err;
  EXPECT_EQ(s_host->worker.options(o, &err), Status::kGuestError);
  EXPECT_NE(err.find("unknown time zone"), std::string::npos) << err;
}

TEST_F(MleBodyForms, JsIntegerResults) {
  struct Case {
    const char *expr;
    std::optional<int64_t> want;  // nullopt: NULL
  };
  for (const Case &c : std::vector<Case>{{"2.5", 3},
                                         {"-2.5", -2},
                                         {"'42'", 42},
                                         {"' 7 '", 7},
                                         {"true", 1},
                                         {"10n", 10},
                                         {"null", std::nullopt},
                                         {"undefined", std::nullopt}}) {
    const Value v = ret(js_expr(c.expr, kInt), {});
    EXPECT_EQ(v, c.want ? Value::i64(*c.want) : Value::null()) << c.expr;
  }
  for (const char *bad :
       {"NaN", "Infinity", "2147483648", "'abc'", "({})", "[1]"}) {
    uint32_t s;
    ResultMsg r = call(js_expr(bad, kInt), {}, &s);
    EXPECT_EQ(s, uint32_t(Status::kGuestError)) << bad;
    ASSERT_TRUE(r.error) << bad;
    EXPECT_EQ(r.error->language, "MLE-Type") << bad;
  }
  ResultMsg r = call(js_expr("NaN", kInt), {});
  EXPECT_EQ(error_of(r), "Cannot convert value 'NaN' to INT from MLE");
  const ParamType tiny = ty(Tag::kI64, "TINYINT", true);
  EXPECT_EQ(ret(js_expr("255", tiny), {}), Value::u64(255));  // unsigned types
  for (const char *bad : {"256", "-1"}) {
    uint32_t s;
    call(js_expr(bad, tiny), {}, &s);
    EXPECT_EQ(s, uint32_t(Status::kGuestError)) << bad;
  }
}

TEST_F(MleBodyForms, JsOtherResults) {
  const ParamType json = ty(Tag::kJson, "JSON");
  EXPECT_EQ(ret(js_expr("({a: 1, b: [true]})", json), {}),
            Value::json("{\"a\":1,\"b\":[true]}"));
  EXPECT_EQ(ret(js_expr("'{\"x\": 2}'", json), {}), Value::json("{\"x\":2}"));
  EXPECT_EQ(ret(js_expr("({toJSON() { return 'me'; }})", json), {}),
            Value::json("\"me\""));
  const ParamType blob = ty(Tag::kBytes, "BLOB");
  EXPECT_EQ(ret(js_expr("new Uint8Array([1, 2, 255])", blob), {}),
            Value::bytes(std::string("\x01\x02\xff", 3)));
  const ParamType dbl = ty(Tag::kF64, "DOUBLE");
  EXPECT_EQ(ret(js_expr("0.5", dbl), {}), Value::f64(0.5));
  EXPECT_EQ(ret(js_expr("'1e3'", dbl), {}), Value::f64(1000));
  const ParamType dec = ty(Tag::kDecimal, "DECIMAL");
  EXPECT_EQ(ret(js_expr("0.1", dec), {}), Value::decimal("0.1"));
  EXPECT_EQ(ret(js_expr("'12.345'", dec), {}), Value::decimal("12.345"));
  EXPECT_EQ(ret(js_expr("({toString() { return 'obj'; }})", kVarchar), {}),
            Value::string("obj"));
  EXPECT_EQ(ret(js_expr("12.5", kVarchar), {}), Value::string("12.5"));
  EXPECT_EQ(ret(js_expr("'01:02:03.5'", ty(Tag::kTime, "TIME")), {}).tag(),
            Tag::kTime);
  EXPECT_EQ(ret(js_expr("new Float32Array([1, 2])", ty(Tag::kVector, "VECTOR")), {}),
            Value::vector({1, 2}));
  for (const auto &[expr, t] : std::vector<std::pair<std::string, ParamType>>{
           {"5", json},
           {"true", json},
           {"'not json'", json},
           {"10n", json},
           {"'abc'", blob},
           {"NaN", dbl},
           {"10n", dbl},
           {"'x'", dec},
           {"'now'", ty(Tag::kDate, "DATE")}}) {
    uint32_t s;
    ResultMsg r = call(js_expr(expr, t), {}, &s);
    EXPECT_EQ(s, uint32_t(Status::kGuestError)) << expr;
    EXPECT_TRUE(r.error && r.error->language == "MLE-Type") << expr << ": " << error_of(r);
  }
}

TEST_F(MleBodyForms, JsErrorPositions) {
  // Line 1: the wrapper text before the body is not counted.
  const uint64_t one = prepare("js", "pos1", "null.foo;", {}, std::nullopt);
  ASSERT_NE(one, 0u);
  ResultMsg r = call(one, {});
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->message.find("TypeError"), std::string::npos) << r.error->message;
  ASSERT_FALSE(r.error->stack.empty());
  EXPECT_EQ(r.error->stack[0].file, "pos1");
  EXPECT_EQ(r.error->stack[0].line, 1u);
  EXPECT_EQ(r.error->stack[0].column, 1u);
  EXPECT_GE(r.error->stack[0].end_column, r.error->stack[0].column);

  const uint64_t two = prepare("js", "pos2",
                               "let x = 1;\nfunction inner() { return null.bar; }\nreturn inner();",
                               {}, kInt);
  ASSERT_NE(two, 0u);
  r = call(two, {});
  ASSERT_TRUE(r.error);
  ASSERT_GE(r.error->stack.size(), 2u);
  EXPECT_EQ(r.error->stack[0].function, "inner");
  EXPECT_EQ(r.error->stack[0].line, 2u);
  EXPECT_EQ(r.error->stack[0].column, 27u);
  EXPECT_EQ(r.error->stack[1].line, 3u);
}

TEST_F(MleBodyForms, JsCompileErrors) {
  std::string err;
  // As Oracle's (GraalJS) messages: name:line:column (from 0), the line, a caret.
  EXPECT_EQ(prepare("js", "syn", "let x = 1;\nreturn x +;", {}, kInt, &err), 0u);
  EXPECT_EQ(err,
            "SyntaxError: syn:2:10 Expected an operand but found ;\n"
            "return x +;\n"
            "          ^\n");
  // On the first line, the wrapper before the body does not show.
  EXPECT_EQ(prepare("js", "syn1", "return +;", {}, kInt, &err), 0u);
  EXPECT_EQ(err,
            "SyntaxError: syn1:1:8 Expected an operand but found ;\n"
            "return +;\n"
            "        ^\n");
  EXPECT_EQ(prepare("js", "bad", "return 1;", {{"a-b", ParamMode::kIn, kInt}},
                    kInt, &err),
            0u);
  EXPECT_NE(err.find("not a JavaScript identifier"), std::string::npos) << err;
}

TEST_F(MleBodyForms, JsFrameLimit) {
  // Oracle: at most 1000 frames of routine code; the routine's own frame
  // counts, the worker's helpers do not.
  const uint64_t id = prepare("js", "recursive_sum",
                              "function sum(n) { return n <= 0 ? 0 : n + sum(n - 1); }\n"
                              "return sum(a);",
                              {{"a", ParamMode::kIn, kInt}}, kBigint);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {Value::i64(998)}), Value::i64(998 * 999 / 2));  // 1000 frames
  uint32_t s;
  ResultMsg r = call(id, {Value::i64(999)}, &s);
  EXPECT_EQ(s, uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(r), "Maximum frame limit of 1000 exceeded. Frames on stack: 1001.");
  ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->language, "js");
  // The count starts again with each call.
  EXPECT_EQ(ret(id, {Value::i64(998)}), Value::i64(998 * 999 / 2));
  r = call(id, {Value::i64(5000)}, &s);
  EXPECT_EQ(error_of(r), "Maximum frame limit of 1000 exceeded. Frames on stack: 1001.");
}

// ---------------------------------------------------------------- Python

TEST_F(MleBodyForms, PythonFunctionBody) {
  const uint64_t id = prepare("python", "padd", "return a + b",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kIn, kInt}},
                              kBigint);
  ASSERT_NE(id, 0u);
  EXPECT_EQ(ret(id, {Value::i64(2), Value::i64(40)}), Value::i64(42));
}

TEST_F(MleBodyForms, PythonIndentedBodies) {
  // As written in SQL: "AS $$ return a $$" or an indented block.
  const uint64_t one = prepare("python", "p1", " return a + 1 ",
                               {{"a", ParamMode::kIn, kInt}}, kInt);
  ASSERT_NE(one, 0u);
  EXPECT_EQ(ret(one, {Value::i64(1)}), Value::i64(2));
  const uint64_t block = prepare("python", "p2",
                                 "\n    x = a * 2\n    if x > 0:\n        return x\n    return 0\n",
                                 {{"a", ParamMode::kIn, kInt}}, kInt);
  ASSERT_NE(block, 0u);
  EXPECT_EQ(ret(block, {Value::i64(21)}), Value::i64(42));
}

TEST_F(MleBodyForms, PythonOutAndInout) {
  std::string err;
  const uint64_t id = prepare("python", "pproc",
                              "assert b is None\n"
                              "a = a + 1\n"
                              "b = a * 2\n"
                              "c = c + '!'\n"
                              "if a > 0:\n"
                              "    return\n"
                              "c = 'not reached'",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kOut, kInt},
                               {"c", ParamMode::kInOut, kVarchar}},
                              std::nullopt, &err);
  ASSERT_NE(id, 0u) << err;
  uint32_t s;
  ResultMsg r = call(id, {Value::i64(20), Value::null(), Value::string("hi")}, &s);
  ASSERT_EQ(s, 0u) << error_of(r);
  ASSERT_EQ(r.out_params.size(), 2u);
  EXPECT_EQ(r.out_params[0].second, Value::i64(42));
  EXPECT_EQ(r.out_params[1].second, Value::string("hi!"));
}

TEST_F(MleBodyForms, PythonErrorsKeepBodyLines) {
  std::string err;
  EXPECT_EQ(prepare("python", "psyn", "x = 1\nreturn (", {}, kInt, &err), 0u);
  EXPECT_EQ(err, "SyntaxError: '(' was never closed (psyn, line 2)");
  const uint64_t id = prepare("python", "pdiv", "x = 1\nreturn x / 0", {}, kInt);
  ASSERT_NE(id, 0u);
  ResultMsg r = call(id, {});
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->message.find("ZeroDivisionError"), std::string::npos);
  ASSERT_FALSE(r.error->stack.empty());
  EXPECT_EQ(r.error->stack[0].file, "pdiv");
  EXPECT_EQ(r.error->stack[0].line, 2u);
}

// ---------------------------------------------------------------- Java

TEST_F(MleBodyForms, JavaMethodBody) {
  std::string err;
  const uint64_t id = prepare("java", "jadd", "return a + b;",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kIn, kBigint}},
                              kBigint, &err);
  ASSERT_NE(id, 0u) << err;
  EXPECT_EQ(ret(id, {Value::i64(2), Value::i64(40)}), Value::i64(42));
  const uint64_t nul = prepare("java", "jnull", "return a == null ? -1 : a;",
                               {{"a", ParamMode::kIn, kInt}}, kInt, &err);
  ASSERT_NE(nul, 0u) << err;
  EXPECT_EQ(ret(nul, {Value::null()}), Value::i64(-1));
  const uint64_t types = prepare(
      "java", "jtypes",
      "return d.add(BigDecimal.ONE) + \"|\" + t.getYear() + \"|\" + v.length;",
      {{"d", ParamMode::kIn, ty(Tag::kDecimal, "DECIMAL")},
       {"t", ParamMode::kIn, ty(Tag::kDatetime, "DATETIME")},
       {"v", ParamMode::kIn, ty(Tag::kVector, "VECTOR")}},
      kVarchar, &err);
  ASSERT_NE(types, 0u) << err;
  EXPECT_EQ(ret(types, {Value::decimal("1.5"), dt(Tag::kDatetime, 2024, 1, 2, 3, 4, 5),
                        Value::vector({1, 2, 3})}),
            Value::string("2.5|2024|3"));
}

TEST_F(MleBodyForms, JavaOutAndInout) {
  std::string err;
  const uint64_t id = prepare("java", "jproc",
                              "if (b[0] != null) throw new IllegalStateException();\n"
                              "b[0] = a * 2;\n"
                              "c[0] = c[0] + \"!\";",
                              {{"a", ParamMode::kIn, kInt},
                               {"b", ParamMode::kOut, kInt},
                               {"c", ParamMode::kInOut, kVarchar}},
                              std::nullopt, &err);
  ASSERT_NE(id, 0u) << err;
  uint32_t s;
  ResultMsg r = call(id, {Value::i64(21), Value::i64(9), Value::string("hi")}, &s);
  ASSERT_EQ(s, 0u) << error_of(r);
  ASSERT_EQ(r.out_params.size(), 2u);
  EXPECT_EQ(r.out_params[0].second, Value::i64(42));
  EXPECT_EQ(r.out_params[1].second, Value::string("hi!"));
}

TEST_F(MleBodyForms, JavaClassForm) {
  const std::string src =
      "import java.util.*;\n"
      "public class Geo {\n"
      "  public static double helper(double x) { return x * x; }\n"
      "  public static double other(double a, double b) { return -1; }\n"
      "  public static double Dist(double a, double b) {\n"
      "    return Math.sqrt(helper(a) + helper(b));\n"
      "  }\n"
      "}";
  std::string err;
  const ParamType dbl = ty(Tag::kF64, "DOUBLE");
  const uint64_t id = prepare("java", "dist", src,
                              {{"a", ParamMode::kIn, dbl}, {"b", ParamMode::kIn, dbl}},
                              dbl, &err);
  ASSERT_NE(id, 0u) << err;
  EXPECT_EQ(ret(id, {Value::f64(3), Value::f64(4)}), Value::f64(5));
  // Two candidates and none named like the routine.
  EXPECT_EQ(prepare("java", "nomatch", src,
                    {{"a", ParamMode::kIn, dbl}, {"b", ParamMode::kIn, dbl}}, dbl,
                    &err),
            0u);
  EXPECT_NE(err.find("name the entry method like the routine"), std::string::npos)
      << err;
}

TEST_F(MleBodyForms, JavaErrorsNameTheRoutine) {
  std::string err;
  EXPECT_EQ(prepare("java", "jsyn", "return a +;", {{"a", ParamMode::kIn, kInt}},
                    kInt, &err),
            0u);
  // The column counts from the start of the body (javac: from 1).
  EXPECT_EQ(err, "jsyn:1:11: illegal start of expression");

  const uint64_t id = prepare("java", "jthrow",
                              "int x = 1;\nthrow new IllegalStateException(\"bad \" + x);",
                              {}, kInt, &err);
  ASSERT_NE(id, 0u) << err;
  ResultMsg r = call(id, {});
  ASSERT_TRUE(r.error);
  EXPECT_NE(r.error->message.find("bad 1"), std::string::npos);
  ASSERT_FALSE(r.error->stack.empty());
  EXPECT_EQ(r.error->stack[0].file, "jthrow");
  EXPECT_EQ(r.error->stack[0].function, "");
  EXPECT_EQ(r.error->stack[0].line, 2u);
}

}  // namespace mle_body_forms_unittest
