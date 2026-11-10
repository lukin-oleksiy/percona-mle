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
// and seccomp). The host side here is a minimal synchronous test host that
// polls the mailbox; the real one (mle::Worker) comes with W4.

#include <gtest/gtest.h>

#include <linux/futex.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "mle/codec.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace mle_java_worker_unittest {

using namespace mle;
using Clock = std::chrono::steady_clock;
using wire::MessageHeader;
using wire::Op;
using wire::Status;

std::string dir_of(const std::string &path) {
  return path.substr(0, path.rfind('/'));
}

ClassConfig jvm_class(const std::string &name, bool prewarm = true) {
  ClassConfig c;
  c.name = name;
  c.executable = MLE_WORKER_EXE;
  c.args = {std::string("--jvm=") + MLE_JAVA_HOME + "/lib/server/libjvm.so",
            std::string("-Djava.class.path=") + MLE_WORKER_JAR,
            "-XX:+UseSerialGC",
            "-Xmx256m",
            "-Xss2m",
            "-XX:CICompilerCount=2",
            "-XX:-UsePerfData",
            "--enable-native-access=ALL-UNNAMED",
            "--add-modules=jdk.incubator.vector",
            "-Dmle.class=" + name};
  if (prewarm) c.args.push_back("-Dmle.prewarm=java");
  if (const char *extra = getenv("MLE_TEST_JVM_OPT")) c.args.push_back(extra);
  if (const char *spin = getenv("MLE_TEST_SPIN"))
    c.args.push_back(std::string("-Dmle.spin=") + spin);
  c.exec_paths = {dir_of(MLE_WORKER_EXE), MLE_JAVA_HOME};
  c.read_paths = {dir_of(MLE_WORKER_JAR)};
  c.inherit_stderr = getenv("MLE_TEST_STDERR") != nullptr;
  return c;
}

// ---------------------------------------------------------------- test host

/** The host's Worker with test hooks; host-side errors are put into the
    result's error so that tests can print them uniformly. */
class TestHost {
 public:
  using SqlHandler = std::function<void(const SqlMsg &, SqlResultMsg *)>;

  explicit TestHost(std::unique_ptr<WorkerProcess> w, WorkerOptions o = {})
      : worker(std::move(w), o) {}

  bool wait_ready(ReadyMsg *m, std::string *err, int timeout_ms = 60000) {
    return worker.wait_ready(timeout_ms, m, err) == Status::kOk;
  }
  const wire::RegionHeader &header() { return *worker.process().header(); }

  uint32_t prepare(const PrepareMsg &p, ResultMsg *res) {
    std::string err;
    const Status s = worker.prepare(p, hooks, res, &err);
    if (!err.empty() && !res->error) res->error = Error{"host", err, {}, 0, ""};
    return uint32_t(s);
  }

  /** sql serves the routine's SQL (kept for nested calls). */
  uint32_t call(uint64_t routine, const std::vector<Value> &args,
                ResultMsg *res, SqlHandler sql = nullptr) {
    if (sql && worker.depth() == 0) hooks.handler = std::move(sql);
    std::string err;
    const Status s = worker.call(routine, 0, args, hooks, res, &err);
    if (!err.empty() && !res->error) res->error = Error{"host", err, {}, 0, ""};
    return uint32_t(s);
  }

  /** INTERRUPT for a given call sequence, sent directly (to test the
      worker's handling of an interrupt that precedes its call). */
  void raw_interrupt(uint64_t call_seq) {
    unsigned char b[9];
    b[0] = uint8_t(wire::Control::kInterrupt);
    memcpy(b + 1, &call_seq, 8);
    EXPECT_EQ(send(worker.process().control_fd(), b, sizeof b, MSG_NOSIGNAL),
              9);
  }

  struct Hooks : CallHooks {
    SqlHandler handler;
    std::atomic<bool> kill{false};
    void sql(const SqlMsg &m, SqlResultMsg *r) override {
      if (handler)
        handler(m, r);
      else
        NoSql().sql(m, r);
    }
    bool killed() override { return kill.load(); }
  } hooks;
  Worker worker;
};

// ---------------------------------------------------------------- fixture

class MleJavaWorker : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    LauncherConfig c;
    c.launcher_exe = MLE_LAUNCHER_EXE;
    c.arena_size = wire::kMinArenaSize;  // 64 KiB: chunking is easy to reach
    c.classes = {jvm_class("jvm"), jvm_class("jvm-cold", false)};
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
    ParamType t;
    t.tag = tag;
    t.is_unsigned = is_unsigned;
    return t;
  }

  static Launcher *s_launcher;
  static TestHost *s_host;
};

Launcher *MleJavaWorker::s_launcher = nullptr;
TestHost *MleJavaWorker::s_host = nullptr;

std::string error_of(const ResultMsg &r) {
  return r.error ? r.error->message : "";
}

// ---------------------------------------------------------------- tests

TEST_F(MleJavaWorker, Ready) {
  ReadyMsg ready;
  TestHost *h = new_host("jvm-cold", &ready);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(ready.worker_class, "jvm-cold");
  EXPECT_EQ(ready.languages, std::vector<std::string>{"java"});
  EXPECT_NE(ready.runtime.find("Java 25"), std::string::npos) << ready.runtime;
  std::cout << "[ INFO     ] " << ready.runtime << ", " << ready.info << "\n";
  EXPECT_EQ(h->header().stats.state, uint32_t(wire::WorkerState::kIdle));
  EXPECT_GT(h->header().stats.heap_max, 0u);
  delete h;
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
      {type(Tag::kF64), Value::f64(std::nan(""))},
      {type(Tag::kF32), Value::f32(3.25f)},
      {type(Tag::kDecimal), Value::decimal("-123456789012345678901234567.50")},
      {type(Tag::kString), Value::string("caf\xc3\xa9 \xf0\x9f\x98\x80")},
      {type(Tag::kBytes), Value::bytes(std::string("a\0b\xff", 4))},
      {type(Tag::kDate), Value::temporal(Tag::kDate, date)},
      {type(Tag::kDatetime), Value::temporal(Tag::kDatetime, dt)},
      {type(Tag::kTimestamp), Value::temporal(Tag::kTimestamp, dt)},
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
            uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(res),
            "argument 1 (p1): value 1099511627776 is out of "
            "range for int");
  EXPECT_EQ(s_host->call(200,
                         {Value::i64(1), Value::null(), Value::decimal("0"),
                          Value::string("")},
                         &res),
            uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("SQL NULL cannot be passed to a double"),
            std::string::npos)
      << error_of(res);

  // A return value that does not fit the SQL type.
  ASSERT_EQ(prepare(*s_host, 201, "R.f",
                    "public class R { public static long f() { return -1; } }",
                    {}, type(Tag::kU64, true), &res),
            0u);
  EXPECT_EQ(s_host->call(201, {}, &res), uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("out of range"), std::string::npos)
      << error_of(res);
}

TEST_F(MleJavaWorker, CompileAndEntryErrors) {
  ResultMsg res;
  EXPECT_EQ(prepare(*s_host, 300, "Bad.f",
                    "public class Bad {\n  public static int f() { return 1 "
                    "}\n}",
                    {}, type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(res), "Bad.java:2:35: ';' expected");

  const std::string src =
      "public class E { public static int f(int a) { return a; } "
      "public static int g(int a) { return a; } "
      "public static int g(String a) { return 0; } }";
  EXPECT_EQ(prepare(*s_host, 301, "E.missing", src, {type(Tag::kI64)},
                    type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_EQ(error_of(res),
            "no public static method missing with 1 parameter in E");
  EXPECT_EQ(prepare(*s_host, 302, "E.g", src, {type(Tag::kI64)},
                    type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("more than one"), std::string::npos);
  EXPECT_EQ(
      prepare(*s_host, 303, "f", src, {type(Tag::kI64)}, type(Tag::kI64), &res),
      uint32_t(Status::kGuestError));
  EXPECT_EQ(prepare(*s_host, 304, "Hidden.f",
                    "class Hidden { public static int f() { return 1; } }", {},
                    type(Tag::kI64), &res),
            uint32_t(Status::kGuestError));
  EXPECT_NE(error_of(res).find("must be public"), std::string::npos)
      << error_of(res);
  // Routines cannot see the worker's internals.
  EXPECT_EQ(prepare(*s_host, 305, "I.f",
                    "public class I { public static String f() { return "
                    "com.percona.mle.worker.Main.class.getName(); } }",
                    {}, type(Tag::kString), &res),
            0u);
  EXPECT_EQ(s_host->call(305, {}, &res), uint32_t(Status::kGuestError));
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
  EXPECT_EQ(s_host->call(400, {}, &res), uint32_t(Status::kGuestError));
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
                    "public class S { public static String f(long k) {\n"
                    "  SqlResult r = Mle.sql(\"SELECT name, v FROM t WHERE id "
                    "= ?\", k);\n"
                    "  try { Mle.sql(\"INSERT INTO t VALUES (1)\"); }\n"
                    "  catch (MleSqlException e) { return r.rows().get(0)[0] + "
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
      r.kind = SqlResultKind::kRows;
      r.columns = {{"name", "t", type(Tag::kString)},
                   {"v", "t", type(Tag::kVector)}};
      r.rows = {Value::string("nine"), Value::vector({1, 2})};
    } else {
      r.kind = SqlResultKind::kError;
      r.sql_errno = 1062;
      r.sqlstate = "23000";
      r.message = "Duplicate entry '1' for key 't.PRIMARY'";
    }
  };
  ASSERT_EQ(s_host->call(500, {Value::i64(9)}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("nine 1062"));

  // An uncaught MySQL error keeps its code and SQLSTATE.
  ASSERT_EQ(
      prepare(*s_host, 501, "U.f",
              "public class U { public static long f() { "
              "com.percona.mle.api.Mle.sql(\"DROP TABLE x\"); return 0; } }",
              {}, type(Tag::kI64), &res),
      0u);
  auto fail = [](const SqlMsg &, SqlResultMsg *r) {
    r->kind = SqlResultKind::kError;
    r->sql_errno = 1051;
    r->sqlstate = "42S02";
    r->message = "Unknown table 'test.x'";
  };
  EXPECT_EQ(s_host->call(501, {}, &res, fail), uint32_t(Status::kGuestError));
  ASSERT_TRUE(res.error.has_value());
  EXPECT_EQ(res.error->sql_errno, 1051u);
  EXPECT_EQ(res.error->sqlstate, "42S02");
  EXPECT_EQ(res.error->message, "Unknown table 'test.x'");
}

// routine -> SQL -> another routine of the same worker -> SQL.
TEST_F(MleJavaWorker, NestedCalls) {
  ResultMsg res;
  ASSERT_EQ(
      prepare(*s_host, 600, "Outer.f",
              "public class Outer { public static String f() { return "
              "\"outer(\" + com.percona.mle.api.Mle.sql(\"SELECT inner()\")"
              ".rows().get(0)[0] + \")\"; } }",
              {}, type(Tag::kString), &res),
      0u);
  ASSERT_EQ(prepare(*s_host, 601, "Inner.f",
                    "public class Inner { public static String f() { return "
                    "\"inner(\" + com.percona.mle.api.Mle.sql(\"SELECT 1\")"
                    ".rows().get(0)[0] + \")\"; } }",
                    {}, type(Tag::kString), &res),
            0u);
  auto sql = [&](const SqlMsg &m, SqlResultMsg *r) {
    r->kind = SqlResultKind::kRows;
    r->columns = {{"c", "", type(Tag::kString)}};
    if (m.statement == "SELECT inner()") {
      EXPECT_EQ(s_host->worker.depth(), 1u);
      ResultMsg inner;
      EXPECT_EQ(s_host->call(601, {}, &inner), 0u) << error_of(inner);
      r->rows = {*inner.ret};
    } else {
      EXPECT_EQ(s_host->worker.depth(), 2u);  // inside the nested call
      r->rows = {Value::string("1")};
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
            uint32_t(Status::kGuestError));
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
  ASSERT_EQ(s_host->worker.drop(900, &err), Status::kOk) << err;
  EXPECT_EQ(s_host->call(900, {}, &res), uint32_t(Status::kInternal));
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
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kInterrupted));
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
                    "public class Z { public static long f() throws Exception "
                    "{ Thread.sleep(60000); return 0; } }",
                    {}, type(Tag::kI64), &res),
            0u);
  h->raw_interrupt(2);  // the next call's sequence number (PREPARE was 1)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto t0 = Clock::now();
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kInterrupted));
  EXPECT_LT(Clock::now() - t0, std::chrono::seconds(5));
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
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kExhausted));
  EXPECT_NE(error_of(res).find("StackOverflowError"), std::string::npos);
  EXPECT_EQ(h->header().stats.tainted, 1u);
  // Retired: killed once the call was over.
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
  EXPECT_EQ(h->call(2, {}, &res), uint32_t(Status::kExhausted));
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
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kKilled));
  killer.join();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
          .count();
  EXPECT_GE(ms, 400);
  EXPECT_LT(ms, 2000);
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
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kInterrupted));
  killer.join();
  EXPECT_LT(Clock::now() - t0, std::chrono::seconds(2));
  EXPECT_TRUE(h->worker.usable());
  delete h;
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
  EXPECT_EQ(h->call(1, {}, &res), uint32_t(Status::kDied));
  EXPECT_EQ(error_of(res), "the MLE worker died: exited with status 7");
  EXPECT_FALSE(h->worker.usable());
  delete h;
}

// routine -> SQL -> the same routine ...: the host stops at max_depth.
TEST_F(MleJavaWorker, NestingLimit) {
  WorkerOptions o;
  o.max_depth = 3;
  TestHost *h = new_host("jvm", nullptr, o);
  ASSERT_NE(h, nullptr);
  ResultMsg res;
  ASSERT_EQ(prepare(*h, 1, "Rec.f",
                    "public class Rec { public static String f() { try { "
                    "return \"(\" + com.percona.mle.api.Mle.sql(\"CALL f()\")"
                    ".rows().get(0)[0] + \")\"; } catch "
                    "(com.percona.mle.api.MleSqlException e) { return "
                    "e.getMessage(); } } }",
                    {}, type(Tag::kString), &res),
            0u);
  auto sql = [h](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg inner;
    const uint32_t st = h->call(1, {}, &inner);
    if (st != 0) {
      r->kind = SqlResultKind::kError;
      r->sql_errno = 1;
      r->sqlstate = "HY000";
      r->message = error_of(inner);
      return;
    }
    r->kind = SqlResultKind::kRows;
    r->columns = {{"c", "", type(Tag::kString)}};
    r->rows = {*inner.ret};
  };
  ASSERT_EQ(h->call(1, {}, &res, sql), 0u) << error_of(res);
  EXPECT_EQ(
      *res.ret,
      Value::string("((too many nested routine calls (the limit is 3)))"));
  delete h;
}

// ---------------------------------------------------------------- speed

// A dedicated worker: the shared one is busy JIT-compiling javac and the
// other tests' routines. The worker's own code needs many calls to reach
// steady state, so warm up long.
TEST_F(MleJavaWorker, CallRoundTrip) {
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
TEST_F(MleJavaWorker, CallRoundTripWithoutSpinning) {
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
TEST_F(MleJavaWorker, JitWarmup) {
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
