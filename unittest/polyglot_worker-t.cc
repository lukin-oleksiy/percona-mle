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

// JS and Python routines in the JVM worker (W5), with Java in the same
// polyglot Context: values, errors, console, SQL, cross-language calls,
// numpy, KILL, lockdown. Under the full lockdown, through mle::Worker.
//
// numpy tests need a GraalPy venv with numpy (CMake MLE_TEST_PYTHON_VENV);
// without it they are skipped.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "jvm_test_util.h"

namespace mle_polyglot_worker_unittest {

using namespace mle;
using namespace mle_test;
using Clock = std::chrono::steady_clock;

class MlePolyglotWorker : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = wire::kMinArenaSize;
    c.classes = {jvm_class("poly", "java,js,python")};
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
    s_host = new_host();
    ASSERT_NE(s_host, nullptr);
  }
  static void TearDownTestSuite() {
    delete s_host;
    delete s_launcher;
  }

  static TestHost *new_host(ReadyMsg *ready = nullptr,
                            WorkerOptions options = {}) {
    std::string err;
    auto w = s_launcher->acquire("poly", &err);
    EXPECT_NE(w, nullptr) << err;
    if (!w) return nullptr;
    auto *h = new TestHost(std::move(w), options);
    ReadyMsg r;
    EXPECT_TRUE(h->wait_ready(&r, &err)) << err;
    if (ready) *ready = r;
    return h;
  }

  static ParamType type(Tag tag, bool is_unsigned = false) {
    ParamType t;
    t.tag = tag;
    t.is_unsigned = is_unsigned;
    return t;
  }

  static uint32_t prepare(TestHost &h, uint64_t id, const std::string &lang,
                          const std::string &entry, const std::string &src,
                          std::vector<ParamType> params,
                          std::optional<ParamType> ret, ResultMsg *res) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = lang;
    p.entry = entry;
    p.source = src;
    for (size_t i = 0; i < params.size(); i++)
      p.params.push_back(
          {"p" + std::to_string(i + 1), wire::ParamMode::kIn, params[i]});
    p.returns = ret;
    return h.prepare(p, res);
  }

  static Launcher *s_launcher;
  static TestHost *s_host;
};

Launcher *MlePolyglotWorker::s_launcher = nullptr;
TestHost *MlePolyglotWorker::s_host = nullptr;

#define NEED_NUMPY()                                                     \
  if (python_venv() == nullptr) {                                        \
    GTEST_SKIP() << "no GraalPy venv with numpy (MLE_TEST_PYTHON_VENV)"; \
  }                                                                      \
  static_assert(true)

// ---------------------------------------------------------------- basics

TEST_F(MlePolyglotWorker, ReadyAndLanguages) {
  ReadyMsg ready;
  const auto t0 = Clock::now();
  TestHost *h = new_host(&ready);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(ready.languages,
            (std::vector<std::string>{"java", "js", "python"}));
  std::cout << "[ INFO     ] worker with java, js and python pre-warmed: "
            << ready.info << " (acquire to READY "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - t0)
                   .count()
            << " ms)\n";
  delete h;
}

TEST_F(MlePolyglotWorker, AddInEachLanguage) {
  ResultMsg res;
  const std::vector<std::pair<std::string, std::string>> sources = {
      {"js", "function add(a, b) { return a + b; }"},
      {"python", "def add(a, b):\n    return a + b\n"},
      {"java",
       "public class A { public static long add(long a, long b) { return a + "
       "b; } }"}};
  uint64_t id = 1;
  for (const auto &[lang, src] : sources) {
    SCOPED_TRACE(lang);
    const std::string entry = lang == "java" ? "A.add" : "add";
    ASSERT_EQ(
        prepare(*s_host, id, lang, entry, src,
                {type(Tag::kI64), type(Tag::kI64)}, type(Tag::kI64), &res),
        0u)
        << error_of(res);
    ASSERT_EQ(s_host->call(id, {Value::i64(40), Value::i64(2)}, &res), 0u)
        << error_of(res);
    EXPECT_EQ(*res.ret, Value::i64(42));
    id++;
  }
}

// Every SQL type through an identity function and back as the same type.
void round_trip(TestHost &h, const std::string &lang, uint64_t first_id) {
  const std::string src = lang == "js" ? "function echo(x) { return x; }"
                                       : "def echo(x):\n    return x\n";
  Temporal date;
  date.year = 2026, date.month = 11, date.day = 10;
  Temporal dt = date;
  dt.hour = 23, dt.minute = 59, dt.second = 58;
  // JS Dates have millisecond precision.
  dt.microsecond = lang == "js" ? 123000 : 123456;
  Temporal time;
  time.hour = 838, time.minute = 59, time.second = 59, time.negative = true;
  Temporal time_us;
  time_us.hour = 1, time_us.minute = 2, time_us.second = 3;
  time_us.microsecond = 456789;
  std::vector<float> v768(768);
  for (size_t i = 0; i < v768.size(); i++) v768[i] = float(i) * 0.25f - 3.0f;
  auto t = [](Tag tag, bool u = false) {
    ParamType p;
    p.tag = tag;
    p.is_unsigned = u;
    return p;
  };
  const std::vector<std::pair<ParamType, Value>> cases = {
      {t(Tag::kI64), Value::i64(-42)},
      {t(Tag::kI64), Value::i64(std::numeric_limits<int64_t>::min())},
      {t(Tag::kU64, true), Value::u64(std::numeric_limits<uint64_t>::max())},
      {t(Tag::kF64), Value::f64(-0.0)},
      {t(Tag::kF64), Value::f64(std::nan(""))},
      {t(Tag::kF64), Value::f64(1e300)},
      {t(Tag::kF32), Value::f32(3.25f)},
      {t(Tag::kDecimal), Value::decimal("-123456789012345678901234567.50")},
      {t(Tag::kString), Value::string("caf\xc3\xa9 \xf0\x9f\x98\x80")},
      {t(Tag::kBytes), Value::bytes(std::string("a\0b\xff", 4))},
      {t(Tag::kDate), Value::temporal(Tag::kDate, date)},
      {t(Tag::kDatetime), Value::temporal(Tag::kDatetime, dt)},
      {t(Tag::kTimestamp), Value::temporal(Tag::kTimestamp, dt)},
      {t(Tag::kTime), Value::temporal(Tag::kTime, time)},
      {t(Tag::kTime), Value::temporal(Tag::kTime, time_us)},
      {t(Tag::kJson), Value::json(R"({"a":[1,2,{"b":null}],"c":"\u00e9"})")},
      {t(Tag::kVector), Value::vector(v768)},
      {t(Tag::kString), Value::null()},
  };
  uint64_t id = first_id;
  for (const auto &[pt, v] : cases) {
    SCOPED_TRACE(lang + " " + tag_name(v.tag()) + " #" + std::to_string(id));
    PrepareMsg p;
    p.routine_id = ++id;
    p.language = lang;
    p.entry = "echo";
    p.source = src;
    p.params = {{"x", wire::ParamMode::kIn, pt}};
    p.returns = pt;
    ResultMsg res;
    ASSERT_EQ(h.prepare(p, &res), 0u) << error_of(res);
    ASSERT_EQ(h.call(id, {v}, &res), 0u) << error_of(res);
    ASSERT_TRUE(res.ret.has_value());
    if (v.tag() == Tag::kJson) {
      // Parsed and serialized again: same document, maybe other escaping.
      EXPECT_EQ(res.ret->tag(), Tag::kJson);
      EXPECT_NE(res.ret->as_text().find("\"b\":null"), std::string::npos)
          << res.ret->as_text();
    } else {
      EXPECT_EQ(*res.ret, v);
    }
  }
}

TEST_F(MlePolyglotWorker, ValuesRoundTripJs) { round_trip(*s_host, "js", 100); }

TEST_F(MlePolyglotWorker, ValuesRoundTripPython) {
  round_trip(*s_host, "python", 200);
}

// What each language sees: the default mapping of SQL types.
TEST_F(MlePolyglotWorker, GuestTypes) {
  ResultMsg res;
  const std::vector<ParamType> params = {type(Tag::kI64),     type(Tag::kI64),
                                         type(Tag::kDecimal), type(Tag::kBytes),
                                         type(Tag::kDate),    type(Tag::kJson),
                                         type(Tag::kVector),  type(Tag::kTime)};
  Temporal date;
  date.year = 2026, date.month = 11, date.day = 10;
  Temporal time;
  time.hour = 1, time.minute = 2, time.second = 3;
  const std::vector<Value> args = {Value::i64(7),
                                   Value::i64(1LL << 60),
                                   Value::decimal("1.50"),
                                   Value::bytes("ab"),
                                   Value::temporal(Tag::kDate, date),
                                   Value::json(R"({"k":[1]})"),
                                   Value::vector({1, 2}),
                                   Value::temporal(Tag::kTime, time)};
  ASSERT_EQ(prepare(*s_host, 300, "js", "types",
                    "function types(...a) { return a.map(x => x === null ? "
                    "'null' : x instanceof Date ? 'Date' : "
                    "Object.prototype.toString.call(x).slice(8, -1) + ':' + "
                    "(typeof x)).join(','); }",
                    params, type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(300, args, &res), 0u) << error_of(res);
  EXPECT_EQ(res.ret->as_text(),
            "Number:number,String:string,String:string,Uint8Array:object,"
            "Date,Object:object,Float32Array:object,String:string");
  ASSERT_EQ(prepare(*s_host, 301, "python", "types",
                    "def types(*a):\n"
                    "    return ','.join(type(x).__module__ + '.' + "
                    "type(x).__name__ for x in a)\n",
                    params, type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(301, args, &res), 0u) << error_of(res);
  const std::string vector_type =
      python_venv() ? "numpy.ndarray" : "array.array";
  EXPECT_EQ(res.ret->as_text(),
            "builtins.int,builtins.int,decimal.Decimal,"
            "builtins.bytes,datetime.date,builtins.dict," +
                vector_type + ",datetime.timedelta");
}

// ---------------------------------------------------------------- errors

TEST_F(MlePolyglotWorker, SyntaxErrors) {
  ResultMsg res;
  EXPECT_EQ(prepare(*s_host, 400, "js", "f", "function f() {\n  return 1 +;\n}",
                    {}, type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(res).rfind("routine_400.js:2:", 0), 0u) << error_of(res);
  EXPECT_EQ(prepare(*s_host, 401, "python", "f", "def f():\n    return 1 +\n",
                    {}, type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("SyntaxError"), std::string::npos)
      << error_of(res);
  EXPECT_NE(error_of(res).find("line 2"), std::string::npos) << error_of(res);
  EXPECT_EQ(prepare(*s_host, 402, "js", "nothere", "function f() {}", {},
                    type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("does not define a function nothere"),
            std::string::npos)
      << error_of(res);
  EXPECT_EQ(prepare(*s_host, 403, "ruby", "f", "", {}, type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
}

TEST_F(MlePolyglotWorker, RuntimeErrorsWithFrames) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 410, "js", "f",
                    "function inner(x) {\n  return x.nope.deeper;\n}\n"
                    "function f() {\n  return inner({});\n}",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(s_host->call(410, {}, &res), uint32_t(Status::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->language, "js");
  EXPECT_NE(res.error->message.find("TypeError"), std::string::npos)
      << res.error->message;
  ASSERT_GE(res.error->stack.size(), 2u);
  EXPECT_EQ(res.error->stack[0].function, "inner");
  EXPECT_EQ(res.error->stack[0].file, "routine_410.js");
  EXPECT_EQ(res.error->stack[0].line, 2u);
  EXPECT_EQ(res.error->stack[1].function, "f");
  EXPECT_EQ(res.error->stack[1].line, 5u);

  ASSERT_EQ(prepare(*s_host, 411, "python", "f",
                    "def inner(x):\n    return 1 / x\n\ndef f():\n"
                    "    return inner(0)\n",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(s_host->call(411, {}, &res), uint32_t(Status::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->language, "python");
  EXPECT_NE(res.error->message.find("ZeroDivisionError"), std::string::npos)
      << res.error->message;
  ASSERT_GE(res.error->stack.size(), 2u);
  EXPECT_EQ(res.error->stack[0].function, "inner");
  EXPECT_EQ(res.error->stack[0].file, "routine_411.py");
  EXPECT_EQ(res.error->stack[0].line, 2u);

  // A result that does not fit the declared type.
  ASSERT_EQ(prepare(*s_host, 412, "js", "f", "function f() { return 'abc'; }",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(s_host->call(412, {}, &res), uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("cannot convert 'abc' to BIGINT"),
            std::string::npos)
      << error_of(res);
}

TEST_F(MlePolyglotWorker, Console) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 420, "js", "f",
                    "function f() { console.log('hello from JS'); "
                    "console.error('oops'); return 1; }",
                    {}, type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(s_host->call(420, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(res.console_out, "hello from JS\n");
  EXPECT_EQ(res.console_err, "oops\n");
  ASSERT_EQ(prepare(*s_host, 421, "python", "f",
                    "import sys\ndef f():\n    print('hello from Python')\n"
                    "    print('oops', file=sys.stderr)\n    return 1\n",
                    {}, type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(s_host->call(421, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(res.console_out, "hello from Python\n");
  EXPECT_EQ(res.console_err, "oops\n");
}

// ---------------------------------------------------------------- SQL

TEST_F(MlePolyglotWorker, SqlFromJsAndPython) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 500, "js", "f",
                    "function f(k) { const r = session.runSql('SELECT name "
                    "FROM t WHERE id = ?', [k]); try { session.runSql('BAD'); } "
                    "catch (e) { return r.fetchOne()[0] + ' ' + e.code; } }",
                    {type(Tag::kI64)}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(prepare(*s_host, 501, "python", "f",
                    "def f(k):\n"
                    "    r = session.run_sql('SELECT name FROM t WHERE id = ?', "
                    "[k])\n"
                    "    try:\n        session.run_sql('BAD')\n"
                    "    except SqlError as e:\n"
                    "        return r.fetch_one()[0] + ' ' + str(e.code)\n",
                    {type(Tag::kI64)}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  auto sql = [&](const SqlMsg &m, SqlResultMsg *r) {
    if (m.statement == "BAD") {
      r->kind = SqlResultKind::kError;
      r->sql_errno = 1064;
      r->sqlstate = "42000";
      r->message = "You have an error in your SQL syntax";
      return;
    }
    EXPECT_EQ(m.params, std::vector<Value>{Value::i64(9)});
    r->kind = SqlResultKind::kRows;
    r->columns = {{"name", "t", type(Tag::kString)}};
    r->rows = {Value::string("nine")};
  };
  for (uint64_t id : {500, 501}) {
    ASSERT_EQ(s_host->call(id, {Value::i64(9)}, &res, sql), 0u)
        << error_of(res);
    EXPECT_EQ(*res.ret, Value::string("nine 1064"));
  }
  // Uncaught: the MySQL error is kept.
  ASSERT_EQ(
      prepare(*s_host, 502, "python", "f", "def f():\n    session.run_sql('BAD')\n", {},
              type(Tag::kI64), &res),
      0u);
  EXPECT_EQ(s_host->call(502, {}, &res), uint32_t(Status::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->sql_errno, 1064u);
  EXPECT_EQ(res.error->sqlstate, "42000");
}

// ---------------------------------------------------------------- polyglot

TEST_F(MlePolyglotWorker, CrossLanguage) {
  ResultMsg res;
  // JS -> Python, Python -> JS, JS -> JDK class.
  ASSERT_EQ(prepare(*s_host, 600, "js", "f",
                    "function f() { const pow = Polyglot.eval('python', "
                    "'lambda a, b: a ** b'); return pow(2, 10) + "
                    "Java.type('java.lang.Math').max(1, 2); }",
                    {}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(600, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(1026));
  ASSERT_EQ(prepare(*s_host, 601, "python", "f",
                    "import polyglot\ndef f(s):\n    up = polyglot.eval("
                    "language='js', string='(s) => s.toUpperCase()')\n"
                    "    return up(s)\n",
                    {type(Tag::kString)}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(601, {Value::string("mle")}, &res), 0u)
      << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("MLE"));
  // Java -> JS through Mle.context().
  ASSERT_EQ(prepare(*s_host, 602, "java", "J.f",
                    "public class J { public static String f() { return "
                    "com.percona.mle.api.Mle.context().eval(\"js\", "
                    "\"[3, 1, 2].sort().join('-')\").asString(); } }",
                    {}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(602, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("1-2-3"));
  // The worker's own classes stay hidden from guests.
  ASSERT_EQ(prepare(*s_host, 603, "js", "f",
                    "function f() { return "
                    "Java.type('com.percona.mle.worker.Main').toString(); }",
                    {}, type(Tag::kString), &res),
            0u);
  EXPECT_EQ(s_host->call(603, {}, &res), uint32_t(Status::kGuestError));
}

TEST_F(MlePolyglotWorker, NumpyInPythonAndFromJava) {
  NEED_NUMPY();
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 700, "python", "cos",
              "import numpy as np\n"
              "def cos(a, b):\n"
              "    return float(a @ b / (np.linalg.norm(a) * "
              "np.linalg.norm(b)))\n",
              {type(Tag::kVector), type(Tag::kVector)}, type(Tag::kF64), &res),
      0u)
      << error_of(res);
  std::vector<float> a(768), b(768);
  for (size_t i = 0; i < a.size(); i++)
    a[i] = float(i % 13), b[i] = float(i % 7);
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); i++)
    dot += a[i] * b[i], na += a[i] * a[i], nb += b[i] * b[i];
  ASSERT_EQ(s_host->call(700, {Value::vector(a), Value::vector(b)}, &res), 0u)
      << error_of(res);
  EXPECT_NEAR(res.ret->as_f64(), dot / std::sqrt(na * nb), 1e-5);
  // A numpy result as VECTOR.
  ASSERT_EQ(prepare(*s_host, 701, "python", "scale",
                    "def scale(v):\n    return v * 2\n", {type(Tag::kVector)},
                    type(Tag::kVector), &res),
            0u);
  ASSERT_EQ(s_host->call(701, {Value::vector({1.5f, -2})}, &res), 0u)
      << error_of(res);
  EXPECT_EQ(*res.ret, Value::vector({3, -4}));
  // A Java routine calling numpy.
  ASSERT_EQ(
      prepare(*s_host, 702, "java", "N.f",
              "public class N { public static double f() { return "
              "com.percona.mle.api.Mle.context().eval(\"python\", "
              "\"import numpy; float(numpy.ones(768) @ numpy.ones(768))\")"
              ".asDouble(); } }",
              {}, type(Tag::kF64), &res),
      0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(702, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::f64(768));
}

// ---------------------------------------------------------------- KILL

TEST_F(MlePolyglotWorker, KillGuestLoops) {
  TestHost *h = new_host();
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "js", "f", "function f() { for (;;) {} }", {},
                    type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(
      prepare(*h, 2, "python", "f", "def f():\n    while True:\n        pass\n",
              {}, type(Tag::kI64), &res),
      0u);
  ASSERT_EQ(prepare(*h, 3, "js", "ok", "function ok() { return 7; }", {},
                    type(Tag::kI64), &res),
            0u);
  for (uint64_t id : {1, 2}) {
    SCOPED_TRACE(id == 1 ? "js" : "python");
    std::thread killer([h] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      h->hooks.kill = true;
    });
    const auto t0 = Clock::now();
    EXPECT_EQ(h->call(id, {}, &res), uint32_t(Status::kInterrupted))
        << error_of(res);
    killer.join();
    h->hooks.kill = false;
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::cout << "[ INFO     ] " << (id == 1 ? "JS" : "Python")
              << " loop stopped " << ms - 200 << " ms after KILL\n";
    EXPECT_LT(ms, 1500);
    // The worker and its context are still usable.
    ASSERT_TRUE(h->worker.usable());
    EXPECT_EQ(h->call(3, {}, &res), 0u) << error_of(res);
  }
  delete h;
}

// ---------------------------------------------------------------- lockdown

TEST_F(MlePolyglotWorker, LockdownFromPython) {
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 800, "python", "f",
              "import os\n"
              "def t(fn):\n"
              "    try:\n"
              "        fn()\n"
              "        return 'OK'\n"
              "    except BaseException as e:\n"
              "        return type(e).__name__\n"
              "def f():\n"
              "    r = []\n"
              "    r.append('read=' + t(lambda: open('/etc/passwd').read()))\n"
              "    r.append('write=' + t(lambda: open('/tmp/mle-py', 'w')))\n"
              "    return ' '.join(r)\n",
              {}, type(Tag::kString), &res),
      0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(800, {}, &res), 0u) << error_of(res);
  const std::string r = res.ret->as_text();
  EXPECT_EQ(r, "read=PermissionError write=PermissionError");
  // Process creation is refused by the Context policy before the OS (seccomp)
  // would refuse it; the routine cannot catch that.
  ASSERT_EQ(prepare(*s_host, 801, "python", "f",
                    "import os\ndef f():\n    return os.system('true')\n", {},
                    type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(s_host->call(801, {}, &res), uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("Process creation is not allowed"),
            std::string::npos)
      << error_of(res);
}

// ---------------------------------------------------------------- speed

void measure(TestHost &h, uint64_t id, const std::vector<Value> &args,
             const char *what) {
  ResultMsg res;
  for (int i = 0; i < 100000; i++) h.call(id, args, &res);
  std::vector<double> ns;
  for (int i = 0; i < 20000; i++) {
    const auto t0 = Clock::now();
    h.call(id, args, &res);
    ns.push_back(
        std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
  }
  std::sort(ns.begin(), ns.end());
  std::cout << "[ INFO     ] " << what << " (after 100k calls): p50 "
            << ns[ns.size() / 2] / 1000 << " us, p90 "
            << ns[ns.size() * 9 / 10] / 1000 << " us\n";
}

TEST_F(MlePolyglotWorker, CallRoundTrip) {
  TestHost *h = new_host();
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  const std::vector<ParamType> p3 = {type(Tag::kI64), type(Tag::kString),
                                     type(Tag::kF64)};
  ASSERT_EQ(prepare(*h, 1, "js", "f",
                    "function f(a, b, c) { return a + b.length + c; }", p3,
                    type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(prepare(*h, 2, "python", "f",
                    "def f(a, b, c):\n    return a + len(b) + int(c)\n", p3,
                    type(Tag::kI64), &res),
            0u);
  const std::vector<Value> args = {Value::i64(1), Value::string("hello"),
                                   Value::f64(2.0)};
  measure(*h, 1, args, "JS call, 3 typed args");
  measure(*h, 2, args, "Python call, 3 typed args");
  // Cross-language calls inside the worker.
  ASSERT_EQ(prepare(*h, 3, "js", "f",
                    "const inc = Polyglot.eval('python', 'lambda x: x + 1');\n"
                    "function f(n) { let s = 0; const t = Date.now(); "
                    "for (let i = 0; i < n; i++) s = inc(s); "
                    "return (Date.now() - t) * 1e6 / n; }",
                    {type(Tag::kI64)}, type(Tag::kF64), &res),
            0u);
  for (int i = 0; i < 5; i++) h->call(3, {Value::i64(200000)}, &res);
  ASSERT_EQ(h->call(3, {Value::i64(1000000)}, &res), 0u) << error_of(res);
  std::cout << "[ INFO     ] JS -> Python call inside the worker: "
            << res.ret->as_f64() << " ns\n";
  delete h;
}

}  // namespace mle_polyglot_worker_unittest
