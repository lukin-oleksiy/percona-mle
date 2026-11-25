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

// Tests of the MLE JVM worker (mle_worker + mle-worker.jar) with Java
// routines, started by the real launcher under the full lockdown (Landlock
// and seccomp). The host's side is mle::Worker (docs/protocol.md; section
// numbers below refer to it).

#include <gtest/gtest.h>

#include <linux/futex.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "jvm_test_util.h"
#include "mle/codec.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_java_worker_unittest {

using namespace mle;
using namespace mle_test;
using Clock = std::chrono::steady_clock;
using wire::MessageHeader;
using wire::Op;

// ---------------------------------------------------------------- fixture

class MleJavaWorker : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = wire::kMinArenaSize;  // 64 KiB: chunking is easy to reach
    c.classes = {jvm_class("jvm", "java", "256m"),
                 jvm_class("jvm_cold", "", "256m")};
    std::string err;
    s_launcher = Launcher::start(c, &err).release();
    ASSERT_NE(s_launcher, nullptr) << err;
    s_host = new_host("jvm");
  }
  static void TearDownTestSuite() {
    delete s_host;
    delete s_launcher;
  }

  static TestHost *new_host(const std::string &cls,
                            ReadyMsg *ready_out = nullptr,
                            WorkerOptions options = {}) {
    std::string err;
    auto w = s_launcher->acquire(cls, &err);
    EXPECT_NE(w, nullptr) << err;
    if (!w) return nullptr;
    auto *h = new TestHost(std::move(w), options);
    ReadyMsg ready;
    EXPECT_TRUE(h->wait_ready(&ready, &err)) << err;
    if (ready_out) *ready_out = ready;
    return h;
  }

  /** PREPAREs source with entry and IN parameters of the given types. */
  static uint32_t prepare(TestHost &h, uint64_t id, const std::string &entry,
                          const std::string &source,
                          std::vector<ParamType> params,
                          std::optional<ParamType> ret, ResultMsg *res) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = "java";
    p.entry = entry;
    p.source = source;
    for (size_t i = 0; i < params.size(); i++)
      p.params.push_back(
          {"p" + std::to_string(i + 1), wire::ParamMode::kIn, params[i]});
    p.returns = ret;
    return h.prepare(p, res);
  }

  static ParamType type(Tag tag, bool is_unsigned = false) {
    return type_of(tag, is_unsigned);
  }

  static Launcher *s_launcher;
  static TestHost *s_host;
};

Launcher *MleJavaWorker::s_launcher = nullptr;
TestHost *MleJavaWorker::s_host = nullptr;

// ---------------------------------------------------------------- tests

TEST_F(MleJavaWorker, Ready) {
  ReadyMsg ready;
  TestHost *h = new_host("jvm_cold", &ready);
  ASSERT_NE(h, nullptr);
  // The worker's own kind, not the name of the host's class (§9.1).
  EXPECT_EQ(ready.worker_class, "jvm");
  EXPECT_EQ(ready.languages,
            (std::vector<std::string>{"java", "js", "python"}));
  EXPECT_NE(ready.runtime.find("Java 25"), std::string::npos) << ready.runtime;
  std::cout << "[ INFO     ] " << ready.runtime << ", " << ready.info << "\n";
  EXPECT_EQ(ready.version, wire::kVersion);
  EXPECT_EQ(ready.features, 0u);
  EXPECT_EQ(h->stats().state, uint32_t(wire::WorkerState::kIdle));
  EXPECT_GT(h->stats().heap_max, 0u);
  delete h;
}

TEST(MleJavaWorkerArgs, StackSizeIsTheWholeValue) {
  // An overflowing --stack was taken as ULLONG_MAX and then not applied (MVP review JVMP-7): the
  // usage error (exit 20) now, before the JVM is looked for.
  auto run = [](const std::string &stack) {
    const std::string cmd = std::string(MLE_WORKER_EXE) + " --jvm=/nonexistent " + stack + " >/dev/null 2>&1";
    const int status = std::system(cmd.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  };
  EXPECT_EQ(run("--stack=99999999999999999999999"), 20);
  EXPECT_EQ(run("--stack=1M"), 20);
  EXPECT_EQ(run("--stack=1024"), 20);       // below 256 KiB
  EXPECT_NE(run("--stack=1048576"), 20);    // accepted: fails later, without a JVM
}

TEST_F(MleJavaWorker, AddTwoLongs) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 1, "Calc.add",
                    "public class Calc { public static long add(long a, long b)"
                    " { return a + b; } }",
                    {type(Tag::kI64), type(Tag::kI64)}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(1, {Value::i64(40), Value::i64(2)}, &res), 0u)
      << error_of(res);
  ASSERT_TRUE(res.ret.has_value());
  EXPECT_EQ(*res.ret, Value::i64(42));
}

// Every SQL type through an Object parameter and back as the same type.
TEST_F(MleJavaWorker, ValuesRoundTrip) {
  const std::string src =
      "package udf; public class Echo { public static Object echo(Object x) "
      "{ return x; } }";
  Temporal date;
  date.year = 2026, date.month = 11, date.day = 10;
  Temporal dt = date;
  dt.hour = 23, dt.minute = 59, dt.second = 58, dt.microsecond = 123456;
  Temporal time;
  time.hour = 838, time.minute = 59, time.second = 59, time.negative = true;
  std::vector<float> v768(768);
  for (size_t i = 0; i < v768.size(); i++) v768[i] = float(i) * 0.25f - 3.0f;
  const std::vector<std::pair<ParamType, Value>> cases = {
      {type(Tag::kI64), Value::i64(std::numeric_limits<int64_t>::min())},
      {type(Tag::kU64, true), Value::u64(std::numeric_limits<uint64_t>::max())},
      {type(Tag::kF64), Value::f64(-0.0)},
      {type(Tag::kF32), Value::f32(3.25f)},
      {type(Tag::kDecimal), Value::decimal("-123456789012345678901234567.50")},
      {type(Tag::kString), Value::string("caf\xc3\xa9 \xf0\x9f\x98\x80")},
      {type(Tag::kBytes), Value::bytes(std::string("a\0b\xff", 4))},
      {type(Tag::kDate), Value::temporal(Tag::kDate, date)},
      {type(Tag::kDatetime), Value::temporal(Tag::kDatetime, dt)},
      // TIMESTAMP is an instant (§7.2): 2024-01-01 00:00:00.123456 UTC and
      // the ends of the wire's range.
      {type(Tag::kTimestamp), Value::timestamp(1704067200123456)},
      {type(Tag::kTimestamp), Value::timestamp(0)},
      {type(Tag::kTimestamp), Value::timestamp(wire::kMaxTimestamp)},
      {type(Tag::kTime), Value::temporal(Tag::kTime, time)},
      {type(Tag::kJson), Value::json(R"({"a": [1, 2]})")},
      {type(Tag::kVector), Value::vector(v768)},
      {type(Tag::kString), Value::null()},
  };
  uint64_t id = 100;
  for (const auto &[t, v] : cases) {
    SCOPED_TRACE(tag_name(v.tag()));
    ResultMsg res;
    ASSERT_EQ(prepare(*s_host, ++id, "Echo.echo", src, {t}, t, &res), 0u)
        << error_of(res);
    ASSERT_EQ(s_host->call(id, {v}, &res), 0u) << error_of(res);
    ASSERT_TRUE(res.ret.has_value());
    EXPECT_EQ(*res.ret, v);
  }
  // MySQL's zero timestamp is on the wire (§7.2), but Java's instant type
  // cannot express it: a guest error (§12), and the worker lives on.
  ResultMsg res;
  const ParamType ts = type(Tag::kTimestamp);
  ASSERT_EQ(prepare(*s_host, ++id, "Echo.echo", src, {ts}, ts, &res), 0u)
      << error_of(res);
  EXPECT_EQ(s_host->call(id, {Value::timestamp(wire::kZeroTimestamp)}, &res),
            u(Outcome::kGuestError));
  EXPECT_EQ(error_of(res),
            "argument 1 (p1): the zero TIMESTAMP has no Java equivalent");
  EXPECT_TRUE(s_host->worker.usable());
}

// NaN and the infinities are not SQL values (§7.2): the host sends none, and
// one that a routine returns fails the call as a conversion error (GUEST_ERROR
// with the language MLE-Type, §9.4), whoever finds it; the worker lives on.
TEST_F(MleJavaWorker, NonFiniteResults) {
  ResultMsg res;
  uint64_t id = 150;
  for (const char *expr :
       {"Double.NaN", "Double.POSITIVE_INFINITY", "Double.NEGATIVE_INFINITY"}) {
    SCOPED_TRACE(expr);
    ASSERT_EQ(prepare(*s_host, ++id, "N.f",
                      std::string("public class N { public static double f() "
                                  "{ return ") +
                          expr + "; } }",
                      {}, type(Tag::kF64), &res),
              0u)
        << error_of(res);
    EXPECT_EQ(s_host->call(id, {}, &res), u(Outcome::kGuestError));
    ASSERT_TRUE(res.error.has_value());
    EXPECT_EQ(res.error->language, wire::kLanguageType);
    EXPECT_FALSE(res.ret.has_value());
    EXPECT_TRUE(s_host->worker.usable());
  }
  ASSERT_EQ(prepare(*s_host, ++id, "N.f",
                    "public class N { public static float[] f() { return new "
                    "float[] {1, Float.NaN}; } }",
                    {}, type(Tag::kVector), &res),
            0u)
      << error_of(res);
  EXPECT_EQ(s_host->call(id, {}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->language, wire::kLanguageType);
  EXPECT_TRUE(s_host->worker.usable());
}

TEST_F(MleJavaWorker, ParameterConversions) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 200, "T.f",
                    "public class T { public static String f(int a, double b, "
                    "java.math.BigDecimal c, String d) { return a + \"/\" + b "
                    "+ \"/\" + c + \"/\" + d; } }",
                    {type(Tag::kI64), type(Tag::kF64), type(Tag::kDecimal),
                     type(Tag::kString)},
                    type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(200,
                         {Value::i64(7), Value::f64(0.5),
                          Value::decimal("1.50"), Value::string("x")},
                         &res),
            0u)
      << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("7/0.5/1.50/x"));

  // Out of range for int, NULL for a primitive: guest errors naming the
  // argument.
  EXPECT_EQ(s_host->call(200,
                         {Value::i64(1LL << 40), Value::f64(0),
                          Value::decimal("0"), Value::string("")},
                         &res),
            u(Outcome::kGuestError));
  EXPECT_EQ(error_of(res),
            "argument 1 (p1): value 1099511627776 is out of "
            "range for int");
  EXPECT_EQ(s_host->call(200,
                         {Value::i64(1), Value::null(), Value::decimal("0"),
                          Value::string("")},
                         &res),
            u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("SQL NULL cannot be passed to a double"),
            std::string::npos)
      << error_of(res);

  // A return value that does not fit the SQL type.
  ASSERT_EQ(prepare(*s_host, 201, "R.f",
                    "public class R { public static long f() { return -1; } }",
                    {}, type(Tag::kU64, true), &res),
            0u);
  EXPECT_EQ(s_host->call(201, {}, &res), u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("out of range"), std::string::npos)
      << error_of(res);
}

TEST_F(MleJavaWorker, CompileAndEntryErrors) {
  ResultMsg res;
  EXPECT_EQ(prepare(*s_host, 300, "Bad.f",
                    "public class Bad {\n  public static int f() { return 1 "
                    "}\n}",
                    {}, type(Tag::kI64), &res),
            u(Outcome::kGuestError));
  EXPECT_EQ(error_of(res), "Bad.java:2:35: ';' expected");

  const std::string src =
      "public class E { public static int f(int a) { return a; } "
      "public static int g(int a) { return a; } "
      "public static int g(String a) { return 0; } }";
  EXPECT_EQ(prepare(*s_host, 301, "E.missing", src, {type(Tag::kI64)},
                    type(Tag::kI64), &res),
            u(Outcome::kGuestError));
  EXPECT_EQ(error_of(res),
            "no public static method missing with 1 parameter in E");
  EXPECT_EQ(prepare(*s_host, 302, "E.g", src, {type(Tag::kI64)},
                    type(Tag::kI64), &res),
            u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("more than one"), std::string::npos);
  EXPECT_EQ(
      prepare(*s_host, 303, "f", src, {type(Tag::kI64)}, type(Tag::kI64), &res),
      u(Outcome::kGuestError));
  EXPECT_EQ(prepare(*s_host, 304, "Hidden.f",
                    "class Hidden { public static int f() { return 1; } }", {},
                    type(Tag::kI64), &res),
            u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("must be public"), std::string::npos)
      << error_of(res);
  // Routines cannot see the worker's internals.
  EXPECT_EQ(prepare(*s_host, 305, "I.f",
                    "public class I { public static String f() { return "
                    "com.percona.mle.worker.Main.class.getName(); } }",
                    {}, type(Tag::kString), &res),
            0u);
  EXPECT_EQ(s_host->call(305, {}, &res), u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("NoClassDefFoundError"), std::string::npos)
      << error_of(res);
}

TEST_F(MleJavaWorker, ExceptionsAndConsole) {
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 400, "Boom.f",
              "public class Boom {\n"
              "  static int depth(int n) {\n"
              "    if (n == 0) throw new IllegalStateException(\"boom\");\n"
              "    return depth(n - 1);\n"
              "  }\n"
              "  public static int f() {\n"
              "    System.out.println(\"hello from Java\");\n"
              "    System.err.print(\"warning\");\n"
              "    return depth(2);\n"
              "  }\n"
              "}",
              {}, type(Tag::kI64), &res),
      0u)
      << error_of(res);
  EXPECT_EQ(s_host->call(400, {}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->language, "java");
  EXPECT_EQ(res.error->message, "java.lang.IllegalStateException: boom");
  ASSERT_EQ(res.error->stack.size(), 4u);  // depth x3, f
  EXPECT_EQ(res.error->stack[0].function, "Boom.depth");
  EXPECT_EQ(res.error->stack[0].file, "Boom.java");
  EXPECT_EQ(res.error->stack[0].line, 3u);
  EXPECT_EQ(res.error->stack[3].function, "Boom.f");
  EXPECT_EQ(res.error->stack[3].line, 9u);
  EXPECT_EQ(res.console_out, "hello from Java\n");
  EXPECT_EQ(res.console_err, "warning");
}

TEST_F(MleJavaWorker, SqlCallback) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 500, "S.f",
                    "import com.percona.mle.api.*;\n"
                    "import com.percona.mle.sql.*;\n"
                    "public class S { public static String f(long k) {\n"
                    "  SqlResult r = Mle.session().runSql(\"SELECT name, v FROM t "
                    "WHERE id = ?\", k);\n"
                    "  try { Mle.session().runSql(\"INSERT INTO t VALUES (1)\"); }\n"
                    "  catch (MleSqlException e) { return r.fetchOne().get(0) + "
                    "\" \" + e.getErrorCode(); }\n"
                    "  return \"no error\";\n"
                    "} }",
                    {type(Tag::kI64)}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  int calls = 0;
  auto sql = [&](const SqlMsg &m, SqlResultMsg *out) {
    SqlResultMsg &r = *out;
    if (calls++ == 0) {
      EXPECT_EQ(m.statement, "SELECT name, v FROM t WHERE id = ?");
      EXPECT_EQ(m.params, std::vector<Value>{Value::i64(9)});
      sql_rows(&r,
               {{"name", "t", type(Tag::kString)},
                {"v", "t", type(Tag::kVector)}},
               {Value::string("nine"), Value::vector({1, 2})});
    } else {
      sql_error(&r, 1062, "23000", "Duplicate entry '1' for key 't.PRIMARY'");
    }
  };
  ASSERT_EQ(s_host->call(500, {Value::i64(9)}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("nine 1062"));

  // An uncaught MySQL error keeps its code and SQLSTATE.
  ASSERT_EQ(
      prepare(*s_host, 501, "U.f",
              "public class U { public static long f() { "
              "com.percona.mle.api.Mle.session().runSql(\"DROP TABLE x\"); return 0; "
              "} }",
              {}, type(Tag::kI64), &res),
      0u);
  auto fail = [](const SqlMsg &, SqlResultMsg *r) {
    sql_error(r, 1051, "42S02", "Unknown table 'test.x'");
  };
  EXPECT_EQ(s_host->call(501, {}, &res, fail), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->sql_errno, 1051u);
  EXPECT_EQ(res.error->sqlstate, "42S02");
  EXPECT_EQ(res.error->message, "Unknown table 'test.x'");
  // The error is handed back by its id (§10.8): the first ERROR of this
  // top-level request, which the host still holds.
  EXPECT_EQ(res.error->sql_error_id, 1u);
  const HeldError *held = s_host->worker.held_error(res.error->sql_error_id);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->sql_errno, 1051u);
  EXPECT_EQ(held->message, "Unknown table 'test.x'");
}

// §10.3: a prepared statement's columns come once. A later result set of the
// id that has exactly those columns carries none in its first batch, and the
// worker uses the ones it received last for the id.
TEST_F(MleJavaWorker, ColumnsOncePerPreparedStatement) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 510, "C.f",
                    "import com.percona.mle.api.*;\n"
                    "import com.percona.mle.sql.*;\n"
                    "public class C { public static String f() {\n"
                    "  PreparedStatement p = Mle.session().prepare(\"SELECT n, s "
                    "FROM t WHERE id = ?\");\n"
                    "  StringBuilder b = new StringBuilder();\n"
                    "  for (long k = 1; k <= 3; k++) {\n"
                    "    SqlResult r = p.bind(k).execute();\n"
                    "    b.append(r.getColumnNames()).append(r.fetchOne().get(1))"
                    ".append(';');\n"
                    "  }\n"
                    "  return b.toString();\n"
                    "} }",
                    {}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  int executions = 0;
  auto sql = [&](const SqlMsg &m, SqlResultMsg *r) {
    if (m.action == wire::SqlAction::kPrepare) {
      *r = SqlResultMsg();
      r->param_count = 1;
      return;
    }
    ASSERT_EQ(m.action, wire::SqlAction::kExecutePrepared);
    const std::string text = "row" + std::to_string(m.params.at(0).as_i64());
    if (executions++ == 0) {
      sql_rows(r, {{"n", "t", type(Tag::kI64)}, {"s", "t", type(Tag::kString)}},
               {Value::i64(1), Value::string(text)});
    } else {
      // The same columns: none are sent.
      sql_rows(r, {}, {Value::i64(2), Value::string(text)});
      r->ncolumns = 2;
      r->total_rows = 1;
    }
  };
  ASSERT_EQ(s_host->call(510, {}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("[n, s]row1;[n, s]row2;[n, s]row3;"));
  EXPECT_EQ(executions, 3);
}

// A first batch without columns for an id that has none: the worker cannot
// tell where a row ends, and exits (§1, item 1).
TEST_F(MleJavaWorker, RowsWithoutColumnsEndTheWorker) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "C.f",
                    "public class C { public static long f() {\n"
                    "  com.percona.mle.api.Mle.session().runSql(\"SELECT 1\");\n"
                    "  return 1; } }",
                    {}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  auto sql = [](const SqlMsg &, SqlResultMsg *r) {
    sql_rows(r, {}, {Value::i64(1)});
    r->ncolumns = 1;
    r->total_rows = 1;
  };
  EXPECT_EQ(h->call(1, {}, &res, sql), u(Outcome::kDied)) << error_of(res);
  EXPECT_FALSE(h->worker.usable());
  delete h;
}

// §1: the worker does not validate a trusted host, but a mistaken one cannot
// make it unsafe. Values outside their domains and ill-formed text give the
// routine some value or an error, or end the worker; a tag that is not the
// column's ends it. Nothing hangs, and the host sees no violation: the
// worker's own messages stay correct.
TEST_F(MleJavaWorker, AMistakenHostDoesNotMakeTheWorkerUnsafe) {
  struct Case {
    const char *what;
    Column column;
    Value value;
    /** The worker must live on (else it may also end itself). */
    bool lives;
  };
  Temporal month13;
  month13.year = 2024;
  month13.month = 13;
  month13.day = 40;
  Temporal late;
  late.hour = 900;
  late.minute = 77;
  const std::vector<Case> cases = {
      {"a DECIMAL that is no number", {"c", "t", type(Tag::kDecimal)},
       Value::decimal("12a.5"), false},
      {"a DECIMAL longer than its limit", {"c", "t", type(Tag::kDecimal)},
       Value::decimal(std::string(300, '7')), true},
      {"a DATE outside its domain", {"c", "t", type(Tag::kDate)},
       Value::temporal(Tag::kDate, month13), true},
      {"a TIME outside its domain", {"c", "t", type(Tag::kTime)},
       Value::temporal(Tag::kTime, late), true},
      {"a TIMESTAMP outside its domain", {"c", "t", type(Tag::kTimestamp)},
       Value::timestamp(wire::kMaxTimestamp + 12345), true},
      {"a column name that is not UTF-8", {"c\xff\xfe", "t", type(Tag::kI64)},
       Value::i64(7), true},
      // Decoded by its own tag (§1 item 2): guest code gets a string.
      {"a tag that is not the column's", {"c", "t", type(Tag::kI64)},
       Value::string("seven"), true},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.what);
    TestHost *h = new_host("jvm");
    ASSERT_NE(h, nullptr);
    ResultMsg res;
    ASSERT_EQ(prepare(*h, 1, "M.f",
                      "import com.percona.mle.api.*;\n"
                      "import com.percona.mle.sql.*;\n"
                      "public class M { public static String f() {\n"
                      "  SqlResult r = Mle.session().runSql(\"SELECT c FROM t\");\n"
                      "  try { return r.getColumnNames() + \" \" + r.fetchOne().get(0); }\n"
                      "  catch (RuntimeException e) { return \"error: \" + e; }\n"
                      "} }",
                      {}, type(Tag::kString), &res),
              0u)
        << error_of(res);
    auto sql = [&c](const SqlMsg &, SqlResultMsg *r) {
      sql_rows(r, {c.column}, {c.value});
      // More that a conforming host would not send.
      r->warnings = {{7, 1, "level 7"}};
    };
    const uint32_t outcome = h->call(1, {}, &res, sql);
    if (c.lives) {
      EXPECT_TRUE(outcome == u(Outcome::kOk) ||
                  outcome == u(Outcome::kGuestError))
          << outcome_name(static_cast<Outcome>(outcome)) << " " << error_of(res);
      EXPECT_TRUE(h->worker.usable()) << h->worker.failure();
      if (res.ret) std::cout << "[ INFO     ] " << c.what << ": "
                             << res.ret->as_text() << "\n";
      // The worker serves on.
      EXPECT_EQ(h->call(1, {}, &res,
                        [](const SqlMsg &, SqlResultMsg *r) {
                          sql_rows(r, {{"c", "t", type(Tag::kI64)}},
                                   {Value::i64(5)});
                        }),
                0u)
          << error_of(res);
      EXPECT_EQ(*res.ret, Value::string("[c] 5"));
    } else {
      // The worker ended itself: it died, the host found no violation.
      EXPECT_EQ(outcome, u(Outcome::kDied))
          << outcome_name(static_cast<Outcome>(outcome)) << " " << error_of(res);
      EXPECT_FALSE(h->worker.usable());
    }
    delete h;
  }
}

// routine -> SQL -> another routine of the same worker -> SQL.
TEST_F(MleJavaWorker, NestedCalls) {
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 600, "Outer.f",
              "public class Outer { public static String f() { return "
              "\"outer(\" + com.percona.mle.api.Mle.session().runSql(\"SELECT "
              "inner()\").fetchOne().get(0) + \")\"; } }",
              {}, type(Tag::kString), &res),
      0u);
  ASSERT_EQ(prepare(*s_host, 601, "Inner.f",
                    "public class Inner { public static String f() { return "
                    "\"inner(\" + com.percona.mle.api.Mle.session().runSql(\"SELECT "
                    "1\").fetchOne().get(0) + \")\"; } }",
                    {}, type(Tag::kString), &res),
            0u);
  auto sql = [&](const SqlMsg &m, SqlResultMsg *r) {
    if (m.statement == "SELECT inner()") {
      EXPECT_EQ(s_host->worker.depth(), 1u);
      ResultMsg inner;
      EXPECT_EQ(s_host->call(601, {}, &inner), 0u) << error_of(inner);
      sql_rows(r, {{"c", "", type(Tag::kString)}}, {*inner.ret});
    } else {
      EXPECT_EQ(s_host->worker.depth(), 2u);  // inside the nested call
      sql_rows(r, {{"c", "", type(Tag::kString)}}, {Value::string("1")});
    }
  };
  ASSERT_EQ(s_host->call(600, {}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("outer(inner(1))"));
}

// VECTOR as a MemorySegment: read with the Vector API without a Java heap
// copy; the segment is closed when the call ends.
TEST_F(MleJavaWorker, VectorSegmentsAndVectorApi) {
  const std::string src =
      "import java.lang.foreign.*; import jdk.incubator.vector.*;\n"
      "public class V {\n"
      "  static final VectorSpecies<Float> S = FloatVector.SPECIES_PREFERRED;\n"
      "  static MemorySegment kept;\n"
      "  public static double dot(MemorySegment a, MemorySegment b) {\n"
      "    if (kept != null) kept.get(ValueLayout.JAVA_FLOAT_UNALIGNED, 0);  "
      "// the previous call's\n"
      "    kept = a;\n"
      "    FloatVector acc = FloatVector.zero(S);\n"
      "    long n = a.byteSize() / 4, i = 0;\n"
      "    for (; i < S.loopBound(n); i += S.length())\n"
      "      acc = FloatVector.fromMemorySegment(S, a, i * 4, "
      "java.nio.ByteOrder.LITTLE_ENDIAN)\n"
      "          .fma(FloatVector.fromMemorySegment(S, b, i * 4, "
      "java.nio.ByteOrder.LITTLE_ENDIAN), acc);\n"
      "    double s = acc.reduceLanes(VectorOperators.ADD);\n"
      "    for (; i < n; i++) s += "
      "a.getAtIndex(ValueLayout.JAVA_FLOAT_UNALIGNED, i) * "
      "b.getAtIndex(ValueLayout.JAVA_FLOAT_UNALIGNED, i);\n"
      "    return s;\n"
      "  }\n"
      "}";
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 700, "V.dot", src,
              {type(Tag::kVector), type(Tag::kVector)}, type(Tag::kF64), &res),
      0u)
      << error_of(res);
  std::vector<float> a(768), b(768);
  double expected = 0;
  for (size_t i = 0; i < a.size(); i++) {
    a[i] = float(i % 7) * 0.5f;
    b[i] = float(i % 5) - 2.0f;
    expected += double(a[i]) * double(b[i]);
  }
  ASSERT_EQ(s_host->call(700, {Value::vector(a), Value::vector(b)}, &res), 0u)
      << error_of(res);
  EXPECT_NEAR(res.ret->as_f64(), expected, 1e-3);

  // The second call touches the segment kept from the first one.
  EXPECT_EQ(s_host->call(700, {Value::vector(a), Value::vector(b)}, &res),
            u(Outcome::kGuestError));
  EXPECT_NE(error_of(res).find("IllegalStateException"), std::string::npos)
      << "a segment kept after the call must be unusable: " << error_of(res);
}

// Arguments and results larger than the 64 KiB arena go in chunks.
TEST_F(MleJavaWorker, ChunkedMessages) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 800, "Big.twice",
                    "public class Big { public static String twice(String s) "
                    "{ return s + s; } }",
                    {type(Tag::kString)}, type(Tag::kString), &res),
            0u);
  std::string big(300 * 1024, 'x');
  for (size_t i = 0; i < big.size(); i += 97) big[i] = char('a' + i % 26);
  ASSERT_EQ(s_host->call(800, {Value::string(big)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string(big + big));
}

TEST_F(MleJavaWorker, DropAndReset) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 900, "D.f",
                    "public class D { public static long f() { return 1; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(s_host->call(900, {}, &res), 0u);
  std::string err;
  ASSERT_EQ(s_host->worker.drop(900, &err), Outcome::kOk) << err;
  // The host sends CALL only for a prepared id (§8.3): nothing is sent.
  EXPECT_FALSE(s_host->worker.is_prepared(900));
  EXPECT_EQ(s_host->call(900, {}, &res), u(Outcome::kRefused));
  // Dropping an id that is not prepared is not an error (§9.6).
  EXPECT_EQ(s_host->worker.drop(900, &err), Outcome::kOk) << err;

  // RESET (§9.7): no id is prepared any more and the options end; the host
  // sends OPTIONS again before it uses the worker.
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ASSERT_EQ(prepare(*h, 1, "D.f",
                    "public class D { public static long f() { return 1; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  ASSERT_EQ(h->worker.reset(&err), Outcome::kOk) << err;
  EXPECT_FALSE(h->worker.is_prepared(1));
  EXPECT_FALSE(h->worker.options_in_force());
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kRefused));
  EXPECT_EQ(prepare(*h, 1, "D.f",
                    "public class D { public static long f() { return 2; } }",
                    {}, type(Tag::kI64), &res),
            u(Outcome::kRefused))
      << "PREPARE without options in force";
  ASSERT_EQ(h->options(OptionsMsg(), &err), Outcome::kOk) << err;
  ASSERT_EQ(prepare(*h, 1, "D.f",
                    "public class D { public static long f() { return 2; } }",
                    {}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(h->call(1, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(2));
  EXPECT_TRUE(h->worker.usable());
  delete h;
}

TEST_F(MleJavaWorker, LockdownFromJava) {
  // Escapes tried by a Java routine, with FFM where Java has no API. The
  // routine may use FFM: the process, not the JVM, is the sandbox.
  const std::string src =
      "import java.lang.foreign.*; import java.lang.invoke.*;\n"
      "public class Esc {\n"
      "  static String t(java.util.concurrent.Callable<Object> c) {\n"
      "    try { return String.valueOf(c.call()); } catch (Throwable e) { "
      "return e.getClass().getSimpleName(); }\n"
      "  }\n"
      "  public static String f(long hostPid) throws Throwable {\n"
      "    Linker l = Linker.nativeLinker();\n"
      "    MethodHandle prctl = "
      "l.downcallHandle(l.defaultLookup().find(\"prctl\").orElseThrow(),\n"
      "        FunctionDescriptor.of(ValueLayout.JAVA_INT, "
      "ValueLayout.JAVA_INT));\n"
      "    MethodHandle fork = "
      "l.downcallHandle(l.defaultLookup().find(\"fork\").orElseThrow(),\n"
      "        FunctionDescriptor.of(ValueLayout.JAVA_INT));\n"
      "    StringBuilder s = new StringBuilder();\n"
      "    s.append(\"dumpable=\").append((int) "
      "prctl.invokeExact(3)).append('\\n');\n"
      "    s.append(\"fork=\").append((int) "
      "fork.invokeExact()).append('\\n');\n"
      "    s.append(\"exec=\").append(t(() -> new "
      "ProcessBuilder(\"/bin/true\").start())).append('\\n');\n"
      "    s.append(\"unix_socket=\").append(t(() -> "
      "java.nio.channels.SocketChannel.open(java.net.StandardProtocolFamily."
      "UNIX))).append('\\n');\n"
      "    s.append(\"tcp=\").append(t(() -> new "
      "java.net.Socket(\"127.0.0.1\", 1))).append('\\n');\n"
      "    s.append(\"signal_host=\").append(t(() -> "
      "ProcessHandle.of(hostPid).map(ProcessHandle::destroy).orElse(null)))."
      "append('\\n');\n"
      "    s.append(\"read_passwd=\").append(t(() -> "
      "java.nio.file.Files.readString(java.nio.file.Path.of(\"/etc/"
      "passwd\")))).append('\\n');\n"
      "    s.append(\"write_tmp=\").append(t(() -> "
      "java.nio.file.Files.writeString(java.nio.file.Path.of(\"/tmp/"
      "mle-esc\"), \"x\"))).append('\\n');\n"
      "    return s.toString();\n"
      "  }\n"
      "}";
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 1000, "Esc.f", src, {type(Tag::kI64)},
                    type(Tag::kString), &res),
            0u)
      << error_of(res);
  ASSERT_EQ(s_host->call(1000, {Value::i64(getpid())}, &res), 0u)
      << error_of(res);
  const std::string r = res.ret->as_text();
  EXPECT_NE(r.find("dumpable=0\n"), std::string::npos) << r;
  EXPECT_NE(r.find("fork=-1\n"), std::string::npos) << r;
  EXPECT_NE(r.find("exec=IOException\n"), std::string::npos) << r;
  // socket() is denied by seccomp, so the JDK's networking classes cannot
  // even initialize: any exception or error is a denial.
  for (const char *probe : {"unix_socket=", "tcp="}) {
    const size_t at = r.find(probe);
    ASSERT_NE(at, std::string::npos) << r;
    const std::string v = r.substr(at, r.find('\n', at) - at);
    EXPECT_TRUE(v.ends_with("Exception") || v.ends_with("Error")) << v;
  }
  EXPECT_NE(r.find("signal_host=null\n"), std::string::npos)
      << r;  // ProcessHandle.of finds no process it may see
  EXPECT_NE(r.find("read_passwd=AccessDeniedException\n"), std::string::npos)
      << r;
  EXPECT_NE(r.find("write_tmp=AccessDeniedException\n"), std::string::npos)
      << r;
  EXPECT_EQ(access("/tmp/mle-esc", F_OK), -1);
}

// ---------------------------------------------------------------- failures

TEST_F(MleJavaWorker, InterruptCooperativeLoop) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "L.f",
                    "public class L { public static long f() { long n = 0; "
                    "while (!Thread.currentThread().isInterrupted()) n++; "
                    "throw new RuntimeException(\"stopped after \" + n); } }",
                    {}, type(Tag::kI64), &res),
            0u);
  std::thread killer([h] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h->hooks.kill = true;  // the session was killed: Worker sends INTERRUPT
  });
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kInterrupted));
  killer.join();
  h->hooks.kill = false;  // the next statement of the session
  std::cout
      << "[ INFO     ] interrupted after "
      << std::chrono::duration<double, std::milli>(Clock::now() - t0).count()
      << " ms\n";
  // The worker is fine afterwards, and the interrupt does not leak into
  // the next call.
  ASSERT_EQ(prepare(*h, 2, "Ok.f",
                    "public class Ok { public static long f() throws Exception "
                    "{ Thread.sleep(20); return 5; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(h->call(2, {}, &res), 0u) << error_of(res);
  delete h;
}

TEST_F(MleJavaWorker, InterruptBeforeTheCallStarts) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Z.f",
                    "public class Z { public static long f(String s) throws "
                    "Exception { Thread.sleep(60000); return 0; } }",
                    {type(Tag::kString)}, type(Tag::kI64), &res),
            0u);
  // An INTERRUPT that reaches the worker before it has received its request
  // completely is kept as pending and delivered when the request begins
  // (§11.1, the worker's table). The host makes the record only once the
  // first chunk is posted, so the request here is a CALL in several chunks
  // (a 64 KiB arena) and the session is found killed between two of them:
  // killed() is polled once before the request starts and then at every
  // chunk.
  h->hooks.kill_at_poll = 2;
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {Value::string(std::string(5 * 64 * 1024, 'x'))}, &res),
            u(Outcome::kInterrupted))
      << error_of(res);
  EXPECT_LT(Clock::now() - t0, std::chrono::seconds(5));
  EXPECT_TRUE(h->worker.interrupted());
  EXPECT_TRUE(h->worker.usable());
  h->hooks.kill_at_poll = 0;
  // The interrupt does not reach the next request.
  ASSERT_EQ(prepare(*h, 2, "Ok.f",
                    "public class Ok { public static long f() { return 5; } }",
                    {}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  EXPECT_EQ(h->call(2, {}, &res), 0u) << error_of(res);
  delete h;
}

TEST_F(MleJavaWorker, ExhaustedTaintsTheWorker) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(
      prepare(*h, 1, "R.f",
              "public class R { static long r(long n) { return r(n + 1) + 1; } "
              "public static long f() { return r(0); } }",
              {}, type(Tag::kI64), &res),
      0u);
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kExhausted));
  EXPECT_NE(error_of(res).find("StackOverflowError"), std::string::npos);
  // Its RESULT had the TAINTED flag: retired, ended once the call was over
  // (§11.5).
  EXPECT_FALSE(h->worker.usable());
  EXPECT_TRUE(h->worker.process().wait_exit(2000));
  delete h;
  h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ASSERT_EQ(prepare(*h, 2, "M.f",
                    "public class M { public static long f() { "
                    "java.util.List<long[]> l = new java.util.ArrayList<>(); "
                    "while (true) l.add(new long[1 << 20]); } }",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(h->call(2, {}, &res), u(Outcome::kExhausted));
  EXPECT_NE(error_of(res).find("OutOfMemoryError"), std::string::npos);
  delete h;
}

TEST_F(MleJavaWorker, KillTightLoopIgnoringInterrupts) {
  WorkerOptions o;
  o.interrupt_grace_ms = 300;
  TestHost *h = new_host("jvm", nullptr, o);
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Spin.f",
                    "public class Spin { static volatile long n; public static "
                    "long f() { while (true) n++; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  std::thread killer([h] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h->hooks.kill = true;
  });
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kKilled));
  killer.join();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
          .count();
  EXPECT_GE(ms, 400);
  EXPECT_LT(ms, 6000);  // generous: sanitizers and parallel runs are slow
  EXPECT_FALSE(h->worker.usable());
  EXPECT_EQ(h->worker.process().exit_info(2000).describe(),
            "killed by signal 9 (Killed)");
  delete h;
}

// The KILL handler of mysqld runs on another thread: Worker::interrupt.
TEST_F(MleJavaWorker, InterruptFromAnotherThread) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Sleep.f",
                    "public class Sleep { public static long f() throws "
                    "Exception { Thread.sleep(60000); return 0; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  std::thread killer([h] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h->worker.interrupt();
  });
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kInterrupted));
  killer.join();
  EXPECT_LT(Clock::now() - t0, std::chrono::seconds(6));
  EXPECT_TRUE(h->worker.usable());
  delete h;
}

// §11.1, the worker's side without a lock on a request's path: a thread that
// asks for interrupts all the time while short calls of Java and JavaScript
// come and go. Every call ends OK or INTERRUPTED (the host ends a worker
// that answers INTERRUPTED without an interrupt), an interrupt never reaches
// a later request, and the worker serves on.
TEST_F(MleJavaWorker, InterruptStateUnderAKillerThread) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Q.f",
                    "public class Q { public static long f(long n) { long s = "
                    "0; for (long i = 0; i < n; i++) s += i; return s; } }",
                    {type(Tag::kI64)}, type(Tag::kI64), &res),
            0u)
      << error_of(res);
  PrepareMsg js;
  js.routine_id = 2;
  js.language = "js";
  js.name = "jsq";
  js.source = "let s = 0; for (let i = 0; i < n; i++) s += i; return s;";
  js.params.push_back({"n", wire::ParamMode::kIn, type(Tag::kI64)});
  js.returns = type(Tag::kI64);
  ASSERT_EQ(h->prepare(js, &res), 0u) << error_of(res);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> asked{0};
  std::thread killer([&] {
    while (!stop.load()) {
      h->worker.interrupt();
      asked++;
      if (asked % 64 == 0) std::this_thread::yield();
    }
  });
  int ok = 0, interrupted = 0;
  for (int i = 0; i < 1500 && h->worker.usable(); i++) {
    const uint64_t id = i % 5 == 0 ? 2 : 1;
    const uint32_t o = h->call(id, {Value::i64(i % 3 == 0 ? 2000 : 10)}, &res);
    if (o == 0) {
      ok++;
    } else {
      ASSERT_EQ(o, u(Outcome::kInterrupted)) << error_of(res);
      EXPECT_TRUE(h->worker.interrupted());
      interrupted++;
    }
  }
  stop = true;
  killer.join();
  ASSERT_TRUE(h->worker.usable()) << h->worker.failure();
  std::cout << "[ INFO     ] " << ok << " calls OK, " << interrupted
            << " interrupted, " << asked.load() << " interrupt() calls\n";
  // Nothing of the storm reaches the calls after it.
  for (int i = 0; i < 50; i++) {
    ASSERT_EQ(h->call(i % 2 == 0 ? 1 : 2, {Value::i64(100)}, &res), 0u)
        << error_of(res);
    EXPECT_EQ(*res.ret, Value::i64(4950));
    EXPECT_FALSE(h->worker.interrupted());
  }
  delete h;
}

// §4.1: a Java routine's own class in a result is converted with its own
// methods before the RESULT is composed; while that runs, SQL is refused, and
// a failure there is a conversion error (MLE-Type), not the worker's.
TEST_F(MleJavaWorker, RoutineClassesInResults) {
  ResultMsg res;
  ASSERT_EQ(prepare(*s_host, 520, "Own.f",
                    "import com.percona.mle.api.*;\n"
                    "public class Own {\n"
                    "  static class Text { public String toString() {\n"
                    "    try { Mle.session().runSql(\"SELECT 1\"); return \"ran SQL\"; }\n"
                    "    catch (RuntimeException e) { return \"no SQL here\"; } } }\n"
                    "  static class Bad { public String toString() {\n"
                    "    throw new IllegalStateException(\"toString failed\"); } }\n"
                    "  public static Object f(long k) { return k == 0 ? new Text() : new Bad(); }\n"
                    "}",
                    {type(Tag::kI64)}, type(Tag::kString), &res),
            0u)
      << error_of(res);
  int statements = 0;
  auto sql = [&](const SqlMsg &, SqlResultMsg *r) {
    statements++;
    r->kind = SqlResultKind::kOk;
  };
  ASSERT_EQ(s_host->call(520, {Value::i64(0)}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("no SQL here"));
  EXPECT_EQ(statements, 0);
  EXPECT_EQ(s_host->call(520, {Value::i64(1)}, &res, sql),
            u(Outcome::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->language, wire::kLanguageType);
  EXPECT_NE(res.error->message.find("toString failed"), std::string::npos)
      << res.error->message;
  EXPECT_TRUE(s_host->worker.usable());
}

TEST_F(MleJavaWorker, WorkerExitsDuringACall) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Halt.f",
                    "public class Halt { public static long f() { "
                    "Runtime.getRuntime().halt(7); return 0; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  EXPECT_EQ(h->call(1, {}, &res), u(Outcome::kDied));
  EXPECT_EQ(error_of(res), "the MLE worker died: exited with status 7");
  EXPECT_FALSE(h->worker.usable());
  delete h;
}

// routine -> SQL -> the same routine ...: depth is at most 31, and the host
// fails the statement that would need a request at depth 32 (§8.2).
TEST_F(MleJavaWorker, NestingLimit) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Rec.f",
                    "public class Rec { public static String f() { try { "
                    "return \"(\" + com.percona.mle.api.Mle.session().runSql(\"CALL "
                    "f()\").fetchOne().get(0) + \")\"; } catch "
                    "(com.percona.mle.api.MleSqlException e) { return "
                    "e.getMessage(); } } }",
                    {}, type(Tag::kString), &res),
            0u);
  uint32_t deepest = 0;
  auto sql = [&](const SqlMsg &, SqlResultMsg *r) {
    deepest = std::max(deepest, h->worker.depth());
    ResultMsg inner;
    const uint32_t st = h->call(1, {}, &inner);
    if (st != 0) {
      EXPECT_EQ(st, u(Outcome::kRefused));
      sql_error(r, 1456, "HY000", error_of(inner));
      return;
    }
    sql_rows(r, {{"c", "", type(Tag::kString)}}, {*inner.ret});
  };
  ASSERT_EQ(h->call(1, {}, &res, sql), 0u) << error_of(res);
  // Requests at depths 0 to 31 ran: 32 in progress when the 33rd is refused.
  EXPECT_EQ(deepest, wire::kMaxDepth + 1);
  EXPECT_EQ(*res.ret,
            Value::string(std::string(wire::kMaxDepth, '(') +
                          "too many nested routine calls (the limit is 32)" +
                          std::string(wire::kMaxDepth, ')')));
  EXPECT_TRUE(h->worker.usable());
  delete h;
}

// ---------------------------------------------------------------- speed

// A dedicated worker: the shared one is busy JIT-compiling javac and the
// other tests' routines. The worker's own code needs many calls to reach
// steady state, so warm up long.
// Repeated calls give the same result each time (the benchmarks below are DISABLED_: run them
// with --gtest_also_run_disabled_tests; MVP review TEST-13).
TEST_F(MleJavaWorker, ManyCalls) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "P.f",
                    "public class P { public static long f(long a, String b, "
                    "double c) { return a + b.length() + (long) c; } }",
                    {type(Tag::kI64), type(Tag::kString), type(Tag::kF64)},
                    type(Tag::kI64), &res),
            0u);
  for (int i = 0; i < 2000; i++) {
    ASSERT_EQ(h->call(1, {Value::i64(i), Value::string("hello"), Value::f64(2.0)}, &res), 0u) << error_of(res);
    ASSERT_EQ(*res.ret, Value::i64(i + 7));
  }
  delete h;
}

TEST_F(MleJavaWorker, DISABLED_CallRoundTrip) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "P.f",
                    "public class P { public static long f(long a, String b, "
                    "double c) { return a + b.length() + (long) c; } }",
                    {type(Tag::kI64), type(Tag::kString), type(Tag::kF64)},
                    type(Tag::kI64), &res),
            0u);
  const std::vector<Value> args = {Value::i64(1), Value::string("hello"),
                                   Value::f64(2.0)};
  auto measure = [&](int n) {
    std::vector<double> ns;
    for (int i = 0; i < n; i++) {
      const auto t0 = Clock::now();
      h->call(1, args, &res);
      ns.push_back(
          std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    return ns;
  };
  for (int round : {1, 2, 3, 4, 5}) {
    const std::vector<double> ns = measure(40000);
    std::cout << "[ INFO     ] Java call, 3 typed args, polling host, after "
              << (round - 1) * 40 << "k calls: p50 " << ns[ns.size() / 2] / 1000
              << " us, p90 " << ns[ns.size() * 9 / 10] / 1000 << " us, p99 "
              << ns[ns.size() * 99 / 100] / 1000 << " us\n";
  }
  EXPECT_EQ(*res.ret, Value::i64(8));
  delete h;
}

// The same call when the host does not spin and sleeps on the futex at
// once: the cost of a sleep and a wake on both sides.
TEST_F(MleJavaWorker, DISABLED_CallRoundTripWithoutSpinning) {
  WorkerOptions o;
  o.spin_us = 0;
  TestHost *h = new_host("jvm", nullptr, o);
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(
      prepare(*h, 1, "P.f",
              "public class P { public static long f(long a) { return a; } }",
              {type(Tag::kI64)}, type(Tag::kI64), &res),
      0u);
  const std::vector<Value> args = {Value::i64(1)};
  for (int i = 0; i < 100000; i++) h->call(1, args, &res);
  std::vector<double> ns;
  for (int i = 0; i < 20000; i++) {
    const auto t0 = Clock::now();
    h->call(1, args, &res);
    ns.push_back(
        std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
  }
  std::sort(ns.begin(), ns.end());
  std::cout << "[ INFO     ] Java call, host sleeps on the futex at once: p50 "
            << ns[ns.size() / 2] / 1000 << " us, p90 "
            << ns[ns.size() * 9 / 10] / 1000 << " us, p99 "
            << ns[ns.size() * 99 / 100] / 1000 << " us\n";
  delete h;
}

// How much JIT warm-up matters: one worker, a compute-heavy routine (cosine
// of two VECTOR(768) arguments, 8 times), latency of call 1, 10, 100, ...
TEST_F(MleJavaWorker, DISABLED_JitWarmup) {
  TestHost *h = new_host("jvm");
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(
      prepare(*h, 1, "Cos.f",
              "public class Cos { public static double f(float[] a, "
              "float[] b) { double s = 0; for (int k = 0; k < 8; k++) { "
              "double d = 0, na = 0, nb = 0; for (int i = 0; i < a.length; "
              "i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * "
              "b[i]; } s += d / Math.sqrt(na * nb); } return s; } }",
              {type(Tag::kVector), type(Tag::kVector)}, type(Tag::kF64), &res),
      0u);
  std::vector<float> a(768), b(768);
  for (size_t i = 0; i < a.size(); i++)
    a[i] = float(i % 13), b[i] = float(i % 7);
  const std::vector<Value> args = {Value::vector(a), Value::vector(b)};
  std::string line;
  for (int i = 1; i <= 100000; i++) {
    const auto t0 = Clock::now();
    ASSERT_EQ(h->call(1, args, &res), 0u);
    const double us =
        std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    if (i == 1 || i == 10 || i == 100 || i == 1000 || i == 10000 || i == 100000)
      line += " #" + std::to_string(i) + "=" + std::to_string(int(us)) + "us";
  }
  std::cout << "[ INFO     ] cosine(768) x8 per call, latency of call" << line
            << "\n";
  delete h;
}

}  // namespace mle_java_worker_unittest
