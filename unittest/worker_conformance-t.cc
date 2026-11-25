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

// The worker conformance suite (exp design note 66, MVP review TEST-2): every worker kind and
// language, started through the real launcher from the build's installed layout, driven by the
// host's Worker with the same cases. A routine is named by its role; a table gives its body per
// language. The arena is the smallest the protocol allows (64 KiB), so that large values cross
// chunk boundaries (§5.2).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>
#include <memory>
#include <string>
#include <vector>

#include "cgroup_test_util.h"
#include "jvm_test_util.h"
#include "mle/defaults.h"
#include "mle/launcher.h"

namespace mle_worker_conformance_unittest {

using namespace mle;
using namespace mle_test;
using wire::ParamMode;

// ---------------------------------------------------------------- engines

enum class Kind { kJvm, kCPython, kV8 };

struct Engine {
  const char *name;
  Kind kind;
  const char *language;
  const char *worker_class;  // READY's
};

const Engine kEngines[] = {
    {"jvm_java", Kind::kJvm, "java", "jvm"},  {"jvm_js", Kind::kJvm, "js", "jvm"},
    {"jvm_python", Kind::kJvm, "python", "jvm"}, {"v8", Kind::kV8, "js", "v8"},
    {"cpython", Kind::kCPython, "python", "cpython"},
};

/** The build's installed layout (plugin_output_directory/mle), as the component has it. */
Layout layout() { return Layout::installed(MLE_DIR, MLE_JAVA_HOME); }

/** The worker class of an engine's kind, or a reason why this build has none. */
std::optional<ClassConfig> class_of(Kind kind, std::string *why) {
  const Layout l = layout();
  JvmClassOptions o;
  o.inherit_stderr = getenv("MLE_TEST_STDERR") != nullptr;
  switch (kind) {
    case Kind::kJvm:
      o.name = "jvm";
      o.prewarm = "java";
      return jvm_worker_class(l, o);
    case Kind::kCPython:
      if (l.cpython_exe.empty()) return *why = "the CPython worker is not built", std::nullopt;
      o.name = "cpython";
      o.memory_max = 512ULL << 20;
      return cpython_worker_class(l, o);
    case Kind::kV8:
      if (l.v8_exe.empty()) return *why = "the V8 worker is not built", std::nullopt;
      o.name = "v8";
      o.memory_max = 512ULL << 20;
      return v8_worker_class(l, o, 256ULL << 20);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- routines

/** A routine's body per language, in the body form of each (Appendix B.3). */
struct Body {
  std::string java, js, python;
  const std::string &of(const std::string &language) const {
    return language == "java" ? java : language == "js" ? js : python;
  }
};

/** Bodies by role. */
const std::map<std::string, Body> kBodies = {
    {"echo", {"return a;", "return a", "return a"}},
    {"out_echo", {"b[0] = a;", "b = a", "b = a"}},
    {"length", {"return (long) a.length();", "return a.length", "return len(a)"}},
    {"text_of_size", {"return \"x\".repeat(a.intValue());", "return 'x'.repeat(a)", "return 'x' * a"}},
    {"second", {"return b;", "return b", "return b"}},
    {"sleep",
     {"try { Thread.sleep(60000); } catch (InterruptedException e) { throw new RuntimeException(e); } return 0L;",
      "mysql.sleep(60); return 0", "import time\ntime.sleep(60)\nreturn 0"}},
    {"sql_value",
     {"return (long) String.valueOf(Mle.session().runSql(\"SELECT v\").fetchOne().get(0)).length();",
      "return String(session.runSql('SELECT v').fetchOne()[0]).length",
      "return len(str(session.run_sql('SELECT v').fetch_one()[0]))"}},
    {"sql_nested",
     {"return ((Number) Mle.session().runSql(\"SELECT nested\").fetchOne().get(0)).longValue();",
      "return session.runSql('SELECT nested').fetchOne()[0]",
      "return session.run_sql('SELECT nested').fetch_one()[0]"}},
    {"huge_sql",
     {"Mle.session().runSql(\"SELECT '\" + \"x\".repeat(65 << 20) + \"'\"); return 0L;",
      "session.runSql(\"SELECT '\" + 'x'.repeat(65 << 20) + \"'\"); return 0",
      "session.run_sql(\"SELECT '\" + 'x' * (65 << 20) + \"'\")\nreturn 0"}},
    {"own_sql_error",
     {"throw new MleSqlException(1644, \"45000\", \"own error\");", "throw new SqlError('45000', 'own error', 1644)",
      "raise SqlError('45000', 'own error', 1644)"}},
    {"hog",
     {"java.util.List<long[]> k = new java.util.ArrayList<>(); while (true) k.add(new long[1 << 20]);",
      "const k = []; for (;;) k.push(new Array(1 << 20).fill(1.5))",
      "k = []\nwhile True:\n    k.append([0.5] * (1 << 20))"}},
};

// ---------------------------------------------------------------- the fixture

class WorkerConformance : public ::testing::TestWithParam<Engine> {
 protected:
  /** One launcher per worker kind for the suite (64 KiB arena). */
  static Launcher *launcher(Kind kind, std::string *why) {
    auto &slot = s_launchers[static_cast<int>(kind)];
    if (slot) return slot.get();
    std::optional<ClassConfig> c = class_of(kind, why);
    if (!c) return nullptr;
    LauncherConfig lc;
    lc.launcher_exe = layout().launcher_exe;
    lc.arena_size = wire::kMinArenaSize;
    lc.classes = {*c};
    std::string err;
    slot = Launcher::start(lc, &err);
    if (!slot) *why = "the launcher did not start: " + err;
    return slot.get();
  }

  void SetUp() override {
    const Engine &e = GetParam();
    std::string why;
    Launcher *l = launcher(e.kind, &why);
    if (l == nullptr) {
      if (why.find("not built") != std::string::npos) MLE_SKIP_OR_FAIL("MLE_TEST_REQUIRE_WORKERS", why);
      FAIL() << why;
    }
    host = start(l, e);
    ASSERT_NE(host, nullptr);
  }

  /** A fresh worker of the engine's kind, READY and with options in force. */
  std::unique_ptr<TestHost> start(Launcher *l, const Engine &e) {
    std::string err;
    auto w = l->acquire(e.worker_class, &err);  // the class is named as the kind (class_of)
    EXPECT_NE(w, nullptr) << err;
    if (!w) return nullptr;
    auto h = std::make_unique<TestHost>(std::move(w));
    EXPECT_TRUE(h->wait_ready(&ready, &err)) << err;
    return h;
  }

  /** PREPAREs the routine of role with params (name, mode, type) and a return type; the status. */
  uint32_t prepare(uint64_t id, const std::string &role, std::vector<std::tuple<std::string, ParamMode, mle::ParamType>> params,
                   std::optional<mle::ParamType> ret, ResultMsg *res) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = GetParam().language;
    p.name = role;
    p.source = kBodies.at(role).of(p.language);
    for (auto &[name, mode, type] : params) p.params.push_back({name, mode, type});
    p.returns = ret;
    return host->prepare(p, res);
  }

  static inline std::unique_ptr<Launcher> s_launchers[3];
  std::unique_ptr<TestHost> host;
  ReadyMsg ready;
};

std::string name_of(const ::testing::TestParamInfo<Engine> &i) { return i.param.name; }

INSTANTIATE_TEST_SUITE_P(Engines, WorkerConformance, ::testing::ValuesIn(kEngines), name_of);

// ---------------------------------------------------------------- 1. READY

TEST_P(WorkerConformance, Ready) {
  const Engine &e = GetParam();
  EXPECT_EQ(ready.version, wire::kVersion);
  EXPECT_EQ(ready.worker_class, e.worker_class);
  EXPECT_NE(std::find(ready.languages.begin(), ready.languages.end(), e.language), ready.languages.end())
      << "READY's languages lack " << e.language;
  EXPECT_FALSE(ready.runtime.empty());
}

// ---------------------------------------------------------------- 2. OPTIONS

TEST_P(WorkerConformance, Options) {
  std::string err;
  Error guest;
  OptionsMsg good;
  good.time_zone = "Europe/Vilnius";
  good.settings = {{"decimal_type", "string"}, {"zz_unknown", "anything"}};  // an unknown name is ignored
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  EXPECT_EQ(host->options(good, &err, &guest), Outcome::kOk) << err << guest.message;

  OptionsMsg zone;
  zone.time_zone = "Nowhere/Atlantis";
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  EXPECT_EQ(host->options(zone, &err, &guest), Outcome::kGuestError);
  EXPECT_EQ(guest.language, std::string(wire::kLanguageTimeZone)) << guest.message;

  OptionsMsg value;
  value.settings = {{"decimal_type", "bogus"}};
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  EXPECT_EQ(host->options(value, &err, &guest), Outcome::kGuestError);
  EXPECT_EQ(guest.language, std::string(wire::kLanguageOption)) << guest.message;
}

// ---------------------------------------------------------------- 3. values

struct Typed {
  const char *what;
  mle::ParamType type;
  Value value;
};

mle::ParamType ty(Tag tag, const char *name, bool is_unsigned = false) {
  mle::ParamType t;
  t.tag = tag;
  t.type_name = name;
  t.is_unsigned = is_unsigned;
  return t;
}

Value temporal(Tag tag, uint16_t y, uint8_t mo, uint8_t d, uint32_t h = 0, uint8_t mi = 0, uint8_t s = 0,
               uint32_t us = 0, bool neg = false) {
  Temporal t;
  t.year = y;
  t.month = mo;
  t.day = d;
  t.hour = h;
  t.minute = mi;
  t.second = s;
  t.microsecond = us;
  t.negative = neg;
  return Value::temporal(tag, t);
}

/** Values of every tag (§7.2) with the edges of §17, under their SQL type. */
std::vector<Typed> values() {
  return {
      {"BIGINT small", ty(Tag::kI64, "BIGINT"), Value::i64(-42)},
      {"BIGINT beyond 2^53", ty(Tag::kI64, "BIGINT"), Value::i64(INT64_MIN)},
      {"BIGINT UNSIGNED max", ty(Tag::kU64, "BIGINT", true), Value::u64(UINT64_MAX)},
      {"DOUBLE", ty(Tag::kF64, "DOUBLE"), Value::f64(0.1)},
      {"DOUBLE -0", ty(Tag::kF64, "DOUBLE"), Value::f64(-0.0)},
      {"FLOAT", ty(Tag::kF32, "FLOAT"), Value::f32(1.5f)},
      {"DECIMAL", ty(Tag::kDecimal, "DECIMAL"), Value::decimal("-12345678901234567890.123456789")},
      {"VARCHAR", ty(Tag::kString, "VARCHAR"), Value::string("h\xC3\xA9llo \xF0\x9F\x98\x80")},
      {"VARCHAR empty", ty(Tag::kString, "VARCHAR"), Value::string("")},
      {"VARBINARY", ty(Tag::kBytes, "VARBINARY"), Value::bytes(std::string("\0\xFF\x01", 3))},
      {"DATE", ty(Tag::kDate, "DATE"), temporal(Tag::kDate, 2024, 2, 29)},
      {"TIME negative", ty(Tag::kTime, "TIME"), temporal(Tag::kTime, 0, 0, 0, 838, 59, 59, 0, true)},
      {"DATETIME", ty(Tag::kDatetime, "DATETIME"), temporal(Tag::kDatetime, 9999, 12, 31, 23, 59, 59, 999000)},
      {"TIMESTAMP", ty(Tag::kTimestamp, "TIMESTAMP"), Value::timestamp(1700000000123000)},
      {"JSON", ty(Tag::kJson, "JSON"), Value::json("{\"a\":[1,2.5,\"x\",null,true]}")},
      {"VECTOR", ty(Tag::kVector, "VECTOR"), Value::vector({1.5f, -0.0f, 3.25f})},
      {"NULL", ty(Tag::kI64, "BIGINT"), Value::null()},
  };
}

TEST_P(WorkerConformance, ValuesRoundTrip) {
  uint64_t id = 100;
  for (const Typed &t : values()) {
    SCOPED_TRACE(t.what);
    ResultMsg res;
    ASSERT_EQ(prepare(++id, "echo", {{"a", ParamMode::kIn, t.type}}, t.type, &res), 0u) << error_of(res);
    ASSERT_EQ(host->call(id, {t.value}, &res), 0u) << error_of(res);
    ASSERT_TRUE(res.ret.has_value());
    EXPECT_EQ(*res.ret, t.value);
    // As an OUT parameter.
    ASSERT_EQ(prepare(++id, "out_echo", {{"a", ParamMode::kIn, t.type}, {"b", ParamMode::kOut, t.type}},
                      std::nullopt, &res),
              0u)
        << error_of(res);
    ASSERT_EQ(host->call(id, {t.value, Value::null()}, &res), 0u) << error_of(res);
    ASSERT_EQ(res.out_params.size(), 1u);
    EXPECT_EQ(res.out_params[0].second, t.value) << "as an OUT parameter";
  }
}

// ---------------------------------------------------------------- 4. chunks

TEST_P(WorkerConformance, ValuesLargerThanTheArena) {
  // 64 KiB arena: a 1 MiB argument and a 1 MiB result cross 16 chunk boundaries each way.
  ResultMsg res;
  const mle::ParamType text = ty(Tag::kString, "LONGTEXT"), big = ty(Tag::kI64, "BIGINT");
  ASSERT_EQ(prepare(1, "length", {{"a", ParamMode::kIn, text}}, big, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {Value::string(std::string(1 << 20, 'a'))}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(1 << 20));
  ASSERT_EQ(prepare(2, "text_of_size", {{"a", ParamMode::kIn, big}}, text, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {Value::i64(1 << 20)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string(std::string(1 << 20, 'x')));
  // A VECTOR of 16000 floats after a 40 KB string: it straddles the first chunk boundary.
  std::vector<float> v(16000);
  for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<float>(i) / 7;
  const mle::ParamType vec = ty(Tag::kVector, "VECTOR");
  ASSERT_EQ(prepare(3, "second", {{"a", ParamMode::kIn, text}, {"b", ParamMode::kIn, vec}}, vec, &res), 0u)
      << error_of(res);
  ASSERT_EQ(host->call(3, {Value::string(std::string(40000, 'p')), Value::vector(v)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::vector(v));
}

// ---------------------------------------------------------------- helpers of cases 5-12

/** An INTERRUPT (a KILL of the host) after ms, from another thread. */
std::thread interrupt_after(TestHost &h, int ms) {
  return std::thread([&h, ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    h.hooks.kill = true;
  });
}

const mle::ParamType kBigint = ty(Tag::kI64, "BIGINT"), kText = ty(Tag::kString, "LONGTEXT");

// ---------------------------------------------------------------- 5. what a worker cannot hold

TEST_P(WorkerConformance, MessagesTheWorkerCannotHold) {
  // §5.3: a worker whose memory is smaller than a host message completes the transfer, fails the
  // request where it belongs and goes on (CONF-8, JVMP-6). The native workers bound host messages
  // by --message-mb (a quarter of memory_max, at least 16 MiB); the JVM worker by its direct
  // memory.
  const Engine &e = GetParam();
  std::string why;
  std::optional<ClassConfig> c = class_of(e.kind, &why);
  ASSERT_TRUE(c) << why;
  if (e.kind == Kind::kJvm) {
    c->args.push_back("-XX:MaxDirectMemorySize=24m");  // every argument of the JVM class is a JVM option
  } else {
    c->args.push_back("--message-mb=16");
  }
  c->name = "small";
  LauncherConfig lc;
  lc.launcher_exe = layout().launcher_exe;
  lc.arena_size = wire::kMinArenaSize;
  lc.classes = {*c};
  std::string err;
  auto l = Launcher::start(lc, &err);
  ASSERT_NE(l, nullptr) << err;
  auto w = l->acquire("small", &err);
  ASSERT_NE(w, nullptr) << err;
  TestHost h(std::move(w));
  ASSERT_TRUE(h.wait_ready(&ready, &err)) << err;
  auto prep = [&](uint64_t id, const std::string &role, std::vector<Param> params, ResultMsg *res) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = e.language;
    p.name = role;
    p.source = kBodies.at(role).of(e.language);
    p.params = std::move(params);
    p.returns = kBigint;
    return h.prepare(p, res);
  };
  ResultMsg res;
  const std::string big(32 << 20, 'a');
  // A CALL argument.
  ASSERT_EQ(prep(1, "length", {{"a", ParamMode::kIn, kText}}, &res), 0u) << error_of(res);
  EXPECT_EQ(h.call(1, {Value::string(big)}, &res), u(Outcome::kExhausted)) << error_of(res);
  // An SQL_RESULT: the statement's error in guest code (the routine does not catch it).
  ASSERT_EQ(prep(2, "sql_value", {}, &res), 0u) << error_of(res);
  EXPECT_EQ(h.call(2, {}, &res,
                   [&](const SqlMsg &, SqlResultMsg *r) {
                     sql_rows(r, {{"v", "", type_of(Tag::kString)}}, {Value::string(big)});
                   }),
            u(Outcome::kGuestError))
      << error_of(res);
  // A PREPARE: EXHAUSTED, and the id is not prepared.
  PrepareMsg p;
  p.routine_id = 3;
  p.language = e.language;
  p.name = "big";
  p.source = kBodies.at("echo").of(e.language) + std::string(e.language == std::string("python") ? "\n#" : "//") + big;
  p.params = {{"a", ParamMode::kIn, kBigint}};
  p.returns = kBigint;
  EXPECT_EQ(h.prepare(p, &res), u(Outcome::kExhausted)) << error_of(res);
  // The worker goes on.
  ASSERT_EQ(h.call(1, {Value::string("four")}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(4));
  EXPECT_TRUE(h.worker.usable());
}

// ---------------------------------------------------------------- 6. nesting

TEST_P(WorkerConformance, NestedCall) {
  // §8.1, §10.6: the SQL of a routine calls a routine of the same worker, served from inside the
  // wait for SQL_RESULT.
  ResultMsg res;
  ASSERT_EQ(prepare(1, "sql_nested", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(prepare(2, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  uint32_t nested_status = 99;
  ASSERT_EQ(host->call(1, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         ResultMsg n;
                         nested_status = host->call(2, {Value::i64(41)}, &n);
                         sql_rows(r, {{"nested", "", type_of(Tag::kI64)}}, {n.ret ? *n.ret : Value::null()});
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(nested_status, 0u);
  EXPECT_EQ(*res.ret, Value::i64(41));
}

// ---------------------------------------------------------------- 7. interrupts

TEST_P(WorkerConformance, Interrupts) {
  ResultMsg res;
  ASSERT_EQ(prepare(1, "sleep", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(prepare(2, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(prepare(3, "sql_nested", {}, kBigint, &res), 0u) << error_of(res);
  // In guest code that waits: INTERRUPTED within the grace period, the worker stays.
  std::thread t = interrupt_after(*host, 200);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(host->call(1, {}, &res), u(Outcome::kInterrupted)) << error_of(res);
  t.join();
  host->hooks.kill = false;
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
  EXPECT_TRUE(host->worker.usable()) << host->worker.failure();
  // After the request ended, the next one is not affected.
  ASSERT_EQ(host->call(2, {Value::i64(5)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(5));
  // In a nested call: the nested request is interrupted, and the outer one stops too.
  uint32_t nested_status = 99;
  t = interrupt_after(*host, 200);
  EXPECT_EQ(host->call(3, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         ResultMsg n;
                         nested_status = host->call(1, {}, &n);
                         sql_error(r, 1317, "70100", "Query execution was interrupted");
                       }),
            u(Outcome::kInterrupted))
      << error_of(res);
  t.join();
  host->hooks.kill = false;
  EXPECT_EQ(nested_status, u(Outcome::kInterrupted));
  ASSERT_EQ(host->call(2, {Value::i64(6)}, &res), 0u) << error_of(res);
}

TEST_P(WorkerConformance, InterruptsOfPythonBeyondALoop) {
  // TEST-14, D11: CPython's blocking waits are woken; a loop that catches BaseException, or a long
  // call into C, is ended by the grace period (the worker is killed).
  const Engine &e = GetParam();
  if (e.kind != Kind::kCPython) GTEST_SKIP() << "the CPython worker's own interrupt paths";
  struct Case {
    const char *body;
    Outcome want;
  };
  uint64_t id = 10;
  for (const Case &c : {Case{"import threading\nthreading.Event().wait(60)\nreturn 0", Outcome::kInterrupted},
                        Case{"import threading\nthreading.Lock().acquire()\nl = threading.Lock()\nl.acquire()\nl.acquire()\nreturn 0",
                             Outcome::kInterrupted},
                        Case{"import time\nwhile True:\n    try:\n        time.sleep(0.01)\n    except BaseException:\n        pass",
                             Outcome::kKilled},
                        Case{"return sum(range(10 ** 12))", Outcome::kKilled}}) {
    SCOPED_TRACE(c.body);
    std::string why;
    if (!host || !host->worker.usable()) host = start(launcher(e.kind, &why), e);
    ASSERT_NE(host, nullptr) << why;
    PrepareMsg p;
    p.routine_id = ++id;
    p.language = e.language;
    p.name = "wait";
    p.source = c.body;
    p.returns = kBigint;
    ResultMsg res;
    ASSERT_EQ(host->prepare(p, &res), 0u) << error_of(res);
    std::thread t = interrupt_after(*host, 200);
    EXPECT_EQ(host->call(id, {}, &res), u(c.want)) << error_of(res);
    t.join();
    host->hooks.kill = false;
  }
}

// ---------------------------------------------------------------- 8. the worker's own limits

TEST_P(WorkerConformance, WorkerRefusesWhatIsTooLarge) {
  // §5.3, §10.6: guest code's SQL over 64 MiB and a RESULT over 64 MiB are refused by the worker
  // itself, as errors of guest code; nothing reaches the host.
  ResultMsg res;
  bool sql_seen = false;
  ASSERT_EQ(prepare(1, "huge_sql", {}, kBigint, &res), 0u) << error_of(res);
  EXPECT_EQ(host->call(1, {}, &res, [&](const SqlMsg &, SqlResultMsg *r) {
              sql_seen = true;
              sql_rows(r, {}, {});
            }),
            u(Outcome::kGuestError))
      << error_of(res);
  EXPECT_FALSE(sql_seen);
  ASSERT_EQ(prepare(2, "text_of_size", {{"a", ParamMode::kIn, kBigint}}, kText, &res), 0u) << error_of(res);
  EXPECT_EQ(host->call(2, {Value::i64(65 << 20)}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error);
  EXPECT_EQ(res.error->language, std::string(wire::kLanguageType)) << res.error->message;
  EXPECT_TRUE(host->worker.usable());
}

// ---------------------------------------------------------------- 10. exhaustion and RESET

TEST_P(WorkerConformance, ExhaustionTaints) {
  const Engine &e = GetParam();
  // Appendix B.5: heap exhausted is EXHAUSTED with TAINTED. CPython has no heap of its own: its
  // limit is the cgroup's, which ends the worker (cpython_memory_limit.test).
  if (e.kind == Kind::kCPython) GTEST_SKIP() << "CPython's memory limit is the cgroup's";
  ResultMsg res;
  ASSERT_EQ(prepare(1, "hog", {}, kBigint, &res), 0u) << error_of(res);
  EXPECT_EQ(host->call(1, {}, &res), u(Outcome::kExhausted)) << error_of(res);
  EXPECT_FALSE(host->worker.usable());
}

TEST_P(WorkerConformance, ResetKeepsAUsableWorker) {
  ResultMsg res;
  std::string err;
  ASSERT_EQ(prepare(1, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {Value::i64(1)}, &res), 0u) << error_of(res);
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  EXPECT_FALSE(host->worker.is_prepared(1));
  ASSERT_EQ(host->options(OptionsMsg(), &err), Outcome::kOk) << err;
  ASSERT_EQ(prepare(1, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {Value::i64(2)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(2));
}

// ---------------------------------------------------------------- 11. a malformed host message

}  // namespace mle_worker_conformance_unittest

namespace mle {
/** Posts what the host's Worker never sends (worker.h's friend). */
struct WorkerTestAccess {
  static void post(Worker &w, wire::Op op, const std::string &body) {
    w.post(op, 0, body.data(), body.size(), body.size());
  }
};
}  // namespace mle

namespace mle_worker_conformance_unittest {

TEST_P(WorkerConformance, MalformedHostMessageEndsTheWorker) {
  // §1, §13: a worker that receives what the protocol does not allow exits non-zero; it does not
  // serve on. Here: a CALL of an id that is not prepared.
  std::string body(8 + 8 + 4, '\0');
  body[0] = 0x7f;  // routine id 127, never prepared
  WorkerTestAccess::post(host->worker, wire::Op::kCall, body);
  ASSERT_TRUE(host->worker.process().wait_exit(10000)) << "the worker did not exit";
  const auto info = host->worker.process().exit_info(1000);
  EXPECT_TRUE(info.known && info.status != 0) << info.describe();
}

// ---------------------------------------------------------------- 12. errors a routine ends with

TEST_P(WorkerConformance, ErrorsARoutineEndsWith) {
  ResultMsg res;
  // Rule 2 of §10.8: the routine's own SQL error.
  ASSERT_EQ(prepare(1, "own_sql_error", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error);
  EXPECT_EQ(res.error->sql_errno, 1644u);
  EXPECT_EQ(res.error->sqlstate, "45000");
  EXPECT_EQ(res.error->sql_error_id, 0u);
  // Rule 1: an ERROR of this request, handed back by its id with its number and SQLSTATE.
  ASSERT_EQ(prepare(2, "sql_value", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res,
                       [](const SqlMsg &, SqlResultMsg *r) { sql_error(r, 1146, "42S02", "no such table"); }),
            u(Outcome::kGuestError));
  ASSERT_TRUE(res.error);
  EXPECT_NE(res.error->sql_error_id, 0u);
  EXPECT_EQ(res.error->sql_errno, 1146u);
  EXPECT_EQ(res.error->sqlstate, "42S02");
  EXPECT_NE(host->worker.held_error(res.error->sql_error_id), nullptr);
}

}  // namespace mle_worker_conformance_unittest
