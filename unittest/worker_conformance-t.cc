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
// chunk boundaries (§5.2). Note 66 §8 completed it (K1-K12): the bytes of §17 from each worker's own
// encoder, values through SQL, ill-formed text, a region of another version, LIBRARY_RESULT and
// SQL_RESULT a worker cannot hold, nesting to the depth limit, statements and errors of earlier
// requests, conversions that run SQL, what RESET keeps.

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <functional>
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
    // ---- note 66 §8 (K2-K12)
    {"select1", {"Mle.session().runSql(\"SELECT 1\"); return 0L;", "session.runSql('SELECT 1'); return 0",
                 "session.run_sql('SELECT 1')\nreturn 0"}},
    // %T: the Java type of the return (JavaRoutines.javaType).
    {"sql_result_echo",
     {"Object o = Mle.session().runSql(\"SELECT v\").fetchOne().get(0);"
      " return (%T) (o instanceof MleJson j ? j.text() : o);",
      "return session.runSql('SELECT v').fetchOne()[0]", "return session.run_sql('SELECT v').fetch_one()[0]"}},
    {"sql_param_echo",
     {"Object o = Mle.session().runSql(\"SELECT ?\", a).fetchOne().get(0);"
      " return (%T) (o instanceof MleJson j ? j.text() : o);",
      "return session.runSql('SELECT ?', [a]).fetchOne()[0]",
      "return session.run_sql('SELECT ?', [a]).fetch_one()[0]"}},
    {"big_sql_text",
     {"Mle.session().runSql(\"SELECT '\" + \"x\".repeat(1 << 20) + \"'\"); return 0L;",
      "session.runSql(\"SELECT '\" + 'x'.repeat(1 << 20) + \"'\"); return 0",
      "session.run_sql(\"SELECT '\" + 'x' * (1 << 20) + \"'\")\nreturn 0"}},
    {"ill_formed_value", {"return \"a\\uD800b\";", "return 'a\\uD800b'", "return 'a\\udc80b'"}},
    {"ill_formed_error",
     {"throw new RuntimeException(\"bad \\uD800 text\");", "throw new Error('bad \\uD800 text')",
      "raise Exception('bad \\udc80 text')"}},
    {"ill_formed_sql",
     {"Mle.session().runSql(\"SELECT '\\uD800'\"); return 0L;", "session.runSql(\"SELECT '\\uD800'\"); return 0",
      "session.run_sql(\"SELECT '\\udc80'\")\nreturn 0"}},
    {"many_params",
     {"Mle.session().runSql(\"SELECT 1\", new Object[70000]); return 0L;",
      "session.runSql('SELECT 1', new Array(70000).fill(1)); return 0",
      "session.run_sql('SELECT 1', [1] * 70000)\nreturn 0"}},
    {"keep_statement",
     {"System.getProperties().put(\"mle.conf.stmt\", Mle.session().prepare(\"SELECT 1\")); return 0L;",
      "Object.getPrototypeOf(globalThis).confStmt = session.prepare('SELECT 1'); return 0",
      "import builtins\nbuiltins.conf_stmt = session.prepare('SELECT 1')\nreturn 0"}},
    {"use_kept_statement",
     {"Object s = System.getProperties().get(\"mle.conf.stmt\"); if (s == null) return 2L;"
      " try { ((PreparedStatement) s).execute(); return 0L; } catch (RuntimeException e) { return 1L; }",
      "const s = Object.getPrototypeOf(globalThis).confStmt\n"
      "if (!s) return 2\n"
      "try { s.execute(); return 0 } catch (e) { return 1 }",
      "import builtins\ns = getattr(builtins, 'conf_stmt', None)\nif s is None:\n    return 2\n"
      "try:\n    s.execute()\n    return 0\nexcept Exception:\n    return 1"}},
    {"keep_error",
     {"try { Mle.session().runSql(\"SELECT v\"); } catch (RuntimeException e) {"
      " System.getProperties().put(\"mle.conf.err\", e); } return 0L;",
      "try { session.runSql('SELECT v') } catch (e) { Object.getPrototypeOf(globalThis).confErr = e } return 0",
      "import builtins\ntry:\n    session.run_sql('SELECT v')\nexcept Exception as e:\n    builtins.conf_err = e\nreturn 0"}},
    {"throw_kept_error",
     {"throw (RuntimeException) System.getProperties().get(\"mle.conf.err\");", "throw Object.getPrototypeOf(globalThis).confErr",
      "import builtins\nraise builtins.conf_err"}},
    {"set_global",
     {"", "Object.getPrototypeOf(globalThis).confKept = 5; return 0",
      "import colorsys, decimal\ndecimal.getcontext().prec = 5\nreturn 0"}},
    {"get_global",
     {"", "const G = Object.getPrototypeOf(globalThis)\nreturn typeof G.confKept === 'number' ? G.confKept : -1",
      "import sys, decimal\nreturn (1 if 'colorsys' in sys.modules else 0) * 100 + (decimal.getcontext().prec != 5)"}},
    {"reenter_param",
     {"", "const v = { toJSON() { session.runSql('SELECT 2'); return 1 }, toString() { session.runSql('SELECT 2'); return '1' } }\n"
          "session.runSql('SELECT ?', [v]); return 0",
      "class V:\n    def __str__(self):\n        session.run_sql('SELECT 2')\n        return '1'\n"
      "session.run_sql('SELECT ?', [V()])\nreturn 0"}},
    {"reenter_result",
     {"", "return { toString() { session.runSql('SELECT 2'); return 'x' } }",
      "class V:\n    def __str__(self):\n        session.run_sql('SELECT 2')\n        return 'x'\nreturn V()"}},
    {"uses_lib", {"return (long) lib.Lib.f();", "return lib.f()", "return lib.f()"}},
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
    if (const size_t at = p.source.find("%T"); at != std::string::npos && ret)
      p.source.replace(at, 2, java_type(*ret));
    for (auto &[name, mode, type] : params) p.params.push_back({name, mode, type});
    p.returns = ret;
    return host->prepare(p, res);
  }

  /** The Java type of a routine's return (JavaRoutines.javaType). */
  static std::string java_type(const mle::ParamType &t) {
    switch (t.tag) {
      case Tag::kI64: return "Long";
      case Tag::kU64: return "java.math.BigInteger";
      case Tag::kF64: return "Double";
      case Tag::kF32: return "Float";
      case Tag::kDecimal: return "java.math.BigDecimal";
      case Tag::kBytes: return "byte[]";
      case Tag::kDate: return "java.time.LocalDate";
      case Tag::kDatetime: return "java.time.LocalDateTime";
      case Tag::kTimestamp: return "java.time.Instant";
      case Tag::kTime: return "java.time.Duration";
      case Tag::kVector: return "float[]";
      default: return "String";
    }
  }

  /** The body of the worker's last message, as it is in the region (§5.1: inline in the worker's line,
      else in the arena); a message of one chunk only. */
  static std::string last_from_worker(TestHost &h) {
    const wire::RegionHeader *rh = h.worker.process().header();
    const uint32_t total = rh->worker.total_len;
    if (wire::is_inline(total)) return std::string(reinterpret_cast<const char *>(rh->worker.body), total);
    return std::string(reinterpret_cast<const char *>(h.worker.process().arena()), rh->worker.len);
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


// ---------------------------------------------------------------- note 66 §8: completion

/** Bytes from hex text ("01 2A 00"), spaces ignored. */
std::string hex_bytes(const std::string &text) {
  std::string out, digits;
  for (char c : text)
    if (isxdigit(static_cast<unsigned char>(c))) digits += c;
  for (size_t i = 0; i + 1 < digits.size(); i += 2) out += static_cast<char>(std::stoi(digits.substr(i, 2), nullptr, 16));
  return out;
}

std::string hex_of(const std::string &b) {
  static const char *d = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : b) {
    if (!out.empty()) out += ' ';
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

/** An SQL_RESULT OK (a statement without a result set). */
void sql_ok(SqlResultMsg *r) {
  *r = SqlResultMsg();
  r->kind = SqlResultKind::kOk;
}

std::string le32(uint32_t v) { return std::string(reinterpret_cast<const char *>(&v), 4); }
std::string le64(uint64_t v) { return std::string(reinterpret_cast<const char *>(&v), 8); }
std::string wstr(const std::string &s) { return le32(static_cast<uint32_t>(s.size())) + s; }

/** A RESULT body (§9.4) with a return value of these bytes and nothing else (§17). */
std::string result_body(const std::string &value) {
  return "\x01" + value + hex_bytes("00 00 00 00  00 00 00 00  00 00 00 00  00  00 00 00 00  00");
}

TEST_P(WorkerConformance, RegionOfAnotherVersion) {
  // K1, §14, CONF-8: a worker given a region of a version it does not speak sends a failed READY
  // with its own version, then exits.
#ifdef NDEBUG
  GTEST_SKIP() << "the launcher writes another region version in debug builds only";
#else
  const Engine &e = GetParam();
  std::string why, err;
  std::optional<ClassConfig> c = class_of(e.kind, &why);
  ASSERT_TRUE(c) << why;
  c->name = "other_version";
  LauncherConfig lc;
  lc.launcher_exe = layout().launcher_exe;
  lc.arena_size = wire::kMinArenaSize;
  lc.classes = {*c};
  lc.test_region_version = 0x00020000;
  auto l = Launcher::start(lc, &err);
  ASSERT_NE(l, nullptr) << err;
  auto w = l->acquire("other_version", &err);
  ASSERT_NE(w, nullptr) << err;
  Worker worker(std::move(w));
  ReadyMsg r;
  EXPECT_NE(worker.wait_ready(60000, &r, &err), Outcome::kOk);
  EXPECT_EQ(r.version, wire::kVersion) << "a failed READY carries the worker's own version; " << err;
  EXPECT_EQ(worker.process().header()->worker.status != 0, true) << "READY's status";
  ASSERT_TRUE(worker.process().wait_exit(10000)) << "the worker did not exit";
#endif
}

TEST_P(WorkerConformance, BytesOfTheTestVectors) {
  // K2, TEST-11: what each worker's own encoder writes, byte for byte, against §17.
  const Engine &e = GetParam();
  std::string err;
  // READY: §17's layout with this worker's own strings.
  {
    std::string why;
    auto w = launcher(e.kind, &why)->acquire(e.worker_class, &err);
    ASSERT_NE(w, nullptr) << err;
    TestHost h(std::move(w));
    ReadyMsg r;
    ASSERT_EQ(h.worker.wait_ready(60000, &r, &err), Outcome::kOk) << err;
    std::string want = le32(r.version) + wstr(r.info) + le64(r.features) + wstr(r.worker_class) +
                       le32(static_cast<uint32_t>(r.languages.size()));
    for (const std::string &lang : r.languages) want += wstr(lang);
    want += wstr(r.runtime);
    EXPECT_EQ(hex_of(last_from_worker(h)), hex_of(want)) << "READY";
    if (e.kind == Kind::kV8) {
      EXPECT_EQ(hex_of(want.substr(0, 4)), "00 00 01 00");
    }
  }
  // RESULT of a function returning I64 42 (28 bytes).
  ResultMsg res;
  ASSERT_EQ(prepare(1, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {Value::i64(42)}, &res), 0u) << error_of(res);
  EXPECT_EQ(hex_of(last_from_worker(*host)),
            "01 01 2A 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00");
  // The values of §17 returned (DECIMAL as text, so that no language turns it into a binary number).
  OptionsMsg exact;
  exact.settings = {{"decimal_type", "string"}};
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  ASSERT_EQ(host->options(exact, &err), Outcome::kOk) << err;
  struct Case {
    const char *what;
    mle::ParamType type;
    Value value;
    const char *bytes;
  };
  const Case cases[] = {
      {"DATE", ty(Tag::kDate, "DATE"), temporal(Tag::kDate, 2024, 2, 29),
       "08 E8 07 02 1D 00 00 00 00 00 00 00 00 00 00 00"},
      {"TIME", ty(Tag::kTime, "TIME"), temporal(Tag::kTime, 0, 0, 0, 1, 2, 3, 500000, true),
       "09 00 00 00 00 01 00 00 00 02 03 20 A1 07 00 01"},
      {"DECIMAL", ty(Tag::kDecimal, "DECIMAL"), Value::decimal("-12.50"), "05 06 00 00 00 2D 31 32 2E 35 30"},
      {"TIMESTAMP", ty(Tag::kTimestamp, "TIMESTAMP"), Value::timestamp(1704067200000000),
       "0B 00 20 21 10 D7 0D 06 00"},
  };
  uint64_t id = 10;
  for (const Case &c : cases) {
    SCOPED_TRACE(c.what);
    ASSERT_EQ(prepare(++id, "echo", {{"a", ParamMode::kIn, c.type}}, c.type, &res), 0u) << error_of(res);
    ASSERT_EQ(host->call(id, {c.value}, &res), 0u) << error_of(res);
    EXPECT_EQ(hex_of(last_from_worker(*host)), hex_of(result_body(hex_bytes(c.bytes))));
  }
  // SQL: EXECUTE "SELECT 1" without params. The statement id, flags, max_rows and row_width are the
  // worker's own (§10); the layout is §17's.
  std::string seen, want;
  ASSERT_EQ(prepare(++id, "select1", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(id, {}, &res,
                       [&](const SqlMsg &m, SqlResultMsg *r) {
                         seen = last_from_worker(*host);
                         want = std::string(1, '\0') + le64(m.stmt_id) + wstr("SELECT 1") + le32(0) +
                                std::string(1, static_cast<char>(m.flags)) + le32(m.max_rows) + le32(m.row_width);
                         sql_rows(r, {{"1", "", type_of(Tag::kI64)}}, {Value::i64(1)});
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(hex_of(seen), hex_of(want)) << "SQL body";
  EXPECT_EQ(seen.size(), 34u);
}

TEST_P(WorkerConformance, ValuesThroughSql) {
  // K3: every tag as an SQL result returned, and as an SQL parameter that comes back as a result.
  std::string err;
  OptionsMsg exact;
  exact.settings = {{"decimal_type", "string"}};
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  ASSERT_EQ(host->options(exact, &err), Outcome::kOk) << err;
  uint64_t id = 100;
  for (const Typed &t : values()) {
    SCOPED_TRACE(t.what);
    ResultMsg res;
    ASSERT_EQ(prepare(++id, "sql_result_echo", {}, t.type, &res), 0u) << error_of(res);
    ASSERT_EQ(host->call(id, {}, &res,
                         [&](const SqlMsg &, SqlResultMsg *r) { sql_rows(r, {{"v", "", t.type}}, {t.value}); }),
              0u)
        << error_of(res);
    ASSERT_TRUE(res.ret.has_value());
    EXPECT_EQ(*res.ret, t.value) << "an SQL result returned";
    Value param;
    ASSERT_EQ(prepare(++id, "sql_param_echo", {{"a", ParamMode::kIn, t.type}}, t.type, &res), 0u) << error_of(res);
    ASSERT_EQ(host->call(id, {t.value}, &res,
                         [&](const SqlMsg &m, SqlResultMsg *r) {
                           param = m.params.empty() ? Value::string("(no parameter)") : m.params[0];
                           mle::ParamType pt = t.type;
                           pt.tag = param.tag() == Tag::kNull ? t.type.tag : param.tag();
                           pt.type_name = sql_type_name(pt.tag, pt.is_unsigned);
                           sql_rows(r, {{"v", "", pt}}, {param});
                         }),
              0u)
        << error_of(res);
    ASSERT_TRUE(res.ret.has_value());
    // JavaScript: a number that is a safe integer goes as BIGINT (Oracle's conversion table, B.4), and
    // Number.isSafeInteger(-0) is true: -0.0 comes back as 0.0. As a value (case 3) -0.0 is kept.
    const bool js_minus_zero = GetParam().language == std::string("js") && t.value == Value::f64(-0.0);
    EXPECT_EQ(*res.ret, js_minus_zero ? Value::f64(0.0) : t.value)
        << "an SQL parameter (sent as " << tag_name(param.tag()) << ") back as a result";
  }
}

TEST_P(WorkerConformance, TextGuestCodeMakesIllFormed) {
  // K4, §7.1: an unpaired surrogate is repaired to U+FFFD in a value and in an error message; in the
  // text of an SQL message it is an error in guest code, and nothing is sent.
  ResultMsg res;
  ASSERT_EQ(prepare(1, "ill_formed_value", {}, kText, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::string("a\xEF\xBF\xBD" "b"));
  ASSERT_EQ(prepare(2, "ill_formed_error", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error);
  EXPECT_NE(res.error->message.find("bad \xEF\xBF\xBD text"), std::string::npos) << res.error->message;
  bool sent = false;
  ASSERT_EQ(prepare(3, "ill_formed_sql", {}, kBigint, &res), 0u) << error_of(res);
  EXPECT_EQ(host->call(3, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         sent = true;
                         sql_ok(r);
                       }),
            u(Outcome::kGuestError))
      << error_of(res);
  EXPECT_FALSE(sent) << "a statement must not be repaired";
  EXPECT_TRUE(host->worker.usable());
}

TEST_P(WorkerConformance, SqlLargerThanTheArena) {
  // K5: an SQL text and a result set larger than the 64 KiB arena.
  ResultMsg res;
  size_t text = 0;
  ASSERT_EQ(prepare(1, "big_sql_text", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {}, &res,
                       [&](const SqlMsg &m, SqlResultMsg *r) {
                         text = m.statement.size();
                         sql_ok(r);
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(text, (1u << 20) + 9);
  ASSERT_EQ(prepare(2, "sql_value", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         sql_rows(r, {{"v", "", type_of(Tag::kString)}}, {Value::string(std::string(1 << 20, 'r'))});
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(1 << 20));
}

TEST_P(WorkerConformance, LibraryTheWorkerCannotHold) {
  // K6, §5.3: a LIBRARY_RESULT larger than the worker can hold fails the import as REFUSED does; no
  // answer is held, the worker goes on and a body it can hold is imported.
  const Engine &e = GetParam();
  std::string why, err;
  std::optional<ClassConfig> c = class_of(e.kind, &why);
  ASSERT_TRUE(c) << why;
  c->args.push_back(e.kind == Kind::kJvm ? "-XX:MaxDirectMemorySize=24m" : "--message-mb=16");
  c->name = "small_lib";
  LauncherConfig lc;
  lc.launcher_exe = layout().launcher_exe;
  lc.arena_size = wire::kMinArenaSize;
  lc.classes = {*c};
  auto l = Launcher::start(lc, &err);
  ASSERT_NE(l, nullptr) << err;
  auto w = l->acquire("small_lib", &err);
  ASSERT_NE(w, nullptr) << err;
  TestHost h(std::move(w));
  ASSERT_TRUE(h.wait_ready(&ready, &err)) << err;
  const std::string lang = e.language;
  const std::string small = lang == "java"   ? "package lib; public final class Lib { public static int f() { return 7; } }"
                            : lang == "js"   ? "export function f() { return 7 }"
                                             : "def f():\n    return 7\n";
  const std::string comment = lang == "python" ? "\n#" : "\n//";
  bool big = true;
  int asked = 0;
  h.hooks.on_library = [&](const LibraryMsg &q, LibraryResultMsg *r) {
    asked++;
    r->status = wire::LibraryStatus::kBody;
    r->language = lang;
    r->body = big ? small + comment + std::string(32 << 20, 'z') : small;
    r->version = big ? 1 : 2;
    EXPECT_EQ(q.known_version, 0u) << "no answer may be held after a body the worker could not hold";
  };
  auto prep = [&](uint64_t id, ResultMsg *res) {
    PrepareMsg p;
    p.routine_id = id;
    p.language = lang;
    p.name = "uses_lib";
    p.source = kBodies.at("uses_lib").of(lang);
    p.returns = kBigint;
    p.imports = {{"test", "lib", ""}};
    return h.prepare(p, res);
  };
  ResultMsg res;
  const uint32_t first = prep(1, &res);
  bool called = false;
  if (first == 0) {  // a worker that imports at the first call, not at PREPARE
    EXPECT_EQ(h.call(1, {}, &res), u(Outcome::kGuestError)) << error_of(res);
    called = true;
  } else {
    EXPECT_EQ(first, u(Outcome::kGuestError)) << error_of(res);
  }
  (void)called;
  EXPECT_GE(asked, 1);
  EXPECT_TRUE(h.worker.usable()) << h.worker.failure();
  big = false;
  ASSERT_EQ(prep(2, &res), 0u) << error_of(res);
  ASSERT_EQ(h.call(2, {}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(7));
}

TEST_P(WorkerConformance, NestingToTheLimit) {
  // K7, §8.1, §10: PREPARE and DROP served while the worker waits for SQL; calls nested to the depth
  // limit; a statement of an earlier request fails in guest code without anything being sent.
  ResultMsg res;
  std::string err;
  ASSERT_EQ(prepare(1, "sql_nested", {}, kBigint, &res), 0u) << error_of(res);
  uint32_t nested_prepare = 99, nested_drop = 99, nested_call = 99;
  ASSERT_EQ(host->call(1, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         ResultMsg n;
                         nested_prepare = prepare(50, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &n);
                         nested_call = host->call(50, {Value::i64(8)}, &n);
                         const Value v = n.ret ? *n.ret : Value::null();
                         nested_drop = u(host->worker.drop(50, &err));
                         sql_rows(r, {{"nested", "", type_of(Tag::kI64)}}, {v});
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(nested_prepare, 0u);
  EXPECT_EQ(nested_call, 0u);
  EXPECT_EQ(nested_drop, 0u) << err;
  EXPECT_EQ(*res.ret, Value::i64(8));
  EXPECT_FALSE(host->worker.is_prepared(50));
  // Depth: routine 1's SQL calls routine 1 again, to depth kMaxDepth.
  int deepest = 0;
  std::function<void(const SqlMsg &, SqlResultMsg *)> deeper = [&](const SqlMsg &, SqlResultMsg *r) {
    const int depth = static_cast<int>(host->worker.depth());
    deepest = std::max(deepest, depth);
    if (depth >= static_cast<int>(wire::kMaxDepth)) {
      sql_rows(r, {{"nested", "", type_of(Tag::kI64)}}, {Value::i64(depth)});
      return;
    }
    ResultMsg n;
    const uint32_t st = host->call(1, {}, &n);
    sql_rows(r, {{"nested", "", type_of(Tag::kI64)}}, {st == 0 && n.ret ? *n.ret : Value::i64(-1)});
  };
  ASSERT_EQ(host->call(1, {}, &res, deeper), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(wire::kMaxDepth)) << "deepest " << deepest;
  host->hooks.handler = nullptr;
  // A statement prepared by an earlier request: an error in guest code (the routine returns 1).
  int sent = 0;
  ASSERT_EQ(prepare(2, "keep_statement", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         *r = SqlResultMsg();
                         r->kind = SqlResultKind::kOk;
                       }),
            0u)
      << error_of(res);
  ASSERT_EQ(prepare(3, "use_kept_statement", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(3, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         sent++;
                         *r = SqlResultMsg();
                         r->kind = SqlResultKind::kOk;
                       }),
            0u)
      << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(1)) << "using an earlier request's statement must fail";
  EXPECT_EQ(sent, 0) << "and send nothing";
}

TEST_P(WorkerConformance, InterruptAtTheStart) {
  // K8, §11.1: an interrupt recorded as the request starts stops it; the next request is not affected.
  ResultMsg res;
  ASSERT_EQ(prepare(1, "sleep", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(prepare(2, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  host->hooks.polls = 0;
  host->hooks.kill_at_poll = 1;
  EXPECT_EQ(host->call(1, {}, &res), u(Outcome::kInterrupted)) << error_of(res);
  host->hooks.kill_at_poll = 0;
  EXPECT_TRUE(host->worker.usable());
  ASSERT_EQ(host->call(2, {Value::i64(3)}, &res), 0u) << error_of(res);
  EXPECT_EQ(*res.ret, Value::i64(3));
}

TEST_P(WorkerConformance, TooManySqlParameters) {
  // K9, §10.6: guest code's SQL with more than 65535 parameters is an error in the worker; nothing is sent.
  ResultMsg res;
  bool sent = false;
  ASSERT_EQ(prepare(1, "many_params", {}, kBigint, &res), 0u) << error_of(res);
  EXPECT_EQ(host->call(1, {}, &res,
                       [&](const SqlMsg &, SqlResultMsg *r) {
                         sent = true;
                         sql_ok(r);
                       }),
            u(Outcome::kGuestError))
      << error_of(res);
  EXPECT_FALSE(sent);
  EXPECT_TRUE(host->worker.usable());
}

TEST_P(WorkerConformance, ConversionThatRunsSql) {
  // K10 (§2 item 9): a value whose conversion runs guest code that runs SQL, as an SQL parameter and
  // as a result: an error in guest code when the conversion runs while the worker writes the message
  // (v8_js.test), else the conversion's SQL runs first and the value is correct; never a corrupted
  // message; the worker goes on.
  const Engine &e = GetParam();
  if (e.language == std::string("java"))
    GTEST_SKIP() << "the Java worker converts only Java's own value types, without running guest code";
  ResultMsg res;
  std::vector<std::string> texts;
  auto record = [&](const SqlMsg &m, SqlResultMsg *r) {
    texts.push_back(m.statement);
    sql_rows(r, {{"x", "", type_of(Tag::kI64)}}, {Value::i64(1)});
  };
  std::vector<Value> params;
  auto record_params = [&](const SqlMsg &m, SqlResultMsg *r) {
    if (m.statement == "SELECT ?") params = m.params;
    record(m, r);
  };
  ASSERT_EQ(prepare(1, "reenter_param", {}, kBigint, &res), 0u) << error_of(res);
  const uint32_t st1 = host->call(1, {}, &res, record_params);
  EXPECT_TRUE(st1 == 0u || st1 == u(Outcome::kGuestError)) << st1 << " " << error_of(res);
  for (const std::string &t : texts) EXPECT_TRUE(t == "SELECT ?" || t == "SELECT 2") << t;
  if (st1 == 0u) {  // converted before the message was written: the outer statement is intact
    ASSERT_EQ(params.size(), 1u);
    EXPECT_TRUE(params[0] == Value::string("1") || params[0] == Value::i64(1) || params[0] == Value::json("1"))
        << tag_name(params[0].tag());
    EXPECT_EQ(texts.back(), "SELECT ?") << "the conversion's SQL runs before the statement it is for";
  }
  host->hooks.handler = nullptr;
  texts.clear();
  ASSERT_EQ(prepare(2, "reenter_result", {}, kText, &res), 0u) << error_of(res);
  const uint32_t st2 = host->call(2, {}, &res, record);
  EXPECT_TRUE(st2 == 0u || st2 == u(Outcome::kGuestError)) << st2 << " " << error_of(res);
  if (st2 == 0u) {
    EXPECT_EQ(*res.ret, Value::string("x"));
  }
  host->hooks.handler = nullptr;
  EXPECT_TRUE(host->worker.usable());
  ASSERT_EQ(prepare(3, "echo", {{"a", ParamMode::kIn, kBigint}}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(3, {Value::i64(9)}, &res), 0u) << error_of(res);
}

TEST_P(WorkerConformance, WhatResetKeeps) {
  // K11, Appendix B.6: JavaScript's global object (jvm, v8; a call's own globalThis is fresh, made
  // from it) and Python's imported modules (jvm, cpython) are kept; the Python decimal context of the
  // routine thread is not.
  const Engine &e = GetParam();
  if (e.language == std::string("java")) GTEST_SKIP() << "B.6 says nothing of Java";
  ResultMsg res;
  std::string err;
  ASSERT_EQ(prepare(1, "set_global", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {}, &res), 0u) << error_of(res);
  ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
  ASSERT_EQ(host->options(OptionsMsg(), &err), Outcome::kOk) << err;
  ASSERT_EQ(prepare(2, "get_global", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res), 0u) << error_of(res);
  if (e.language == std::string("js")) {
    EXPECT_EQ(*res.ret, Value::i64(5)) << "the global object kept";
    // A loaded library and its module state: the worker asks again after RESET (its answers are
    // dropped, §9.10) and, told UNCHANGED, goes on with the module it has.
    host->hooks.on_library = [](const LibraryMsg &q, LibraryResultMsg *r) {
      r->language = "js";
      r->version = 7;
      if (q.known_version == 7) {
        r->status = wire::LibraryStatus::kUnchanged;
      } else {
        r->status = wire::LibraryStatus::kBody;
        r->body = "let n = 0\nexport function f() { return ++n }";
      }
    };
    auto prep_lib = [&](uint64_t id) {
      PrepareMsg p;
      p.routine_id = id;
      p.language = "js";
      p.name = "uses_lib";
      p.source = kBodies.at("uses_lib").js;
      p.returns = kBigint;
      p.imports = {{"test", "lib", ""}};
      return host->prepare(p, &res);
    };
    ASSERT_EQ(prep_lib(3), 0u) << error_of(res);
    ASSERT_EQ(host->call(3, {}, &res), 0u) << error_of(res);
    EXPECT_EQ(*res.ret, Value::i64(1));
    ASSERT_EQ(host->worker.reset(&err), Outcome::kOk) << err;
    ASSERT_EQ(host->options(OptionsMsg(), &err), Outcome::kOk) << err;
    ASSERT_EQ(prep_lib(4), 0u) << error_of(res);
    ASSERT_EQ(host->call(4, {}, &res), 0u) << error_of(res);
    EXPECT_EQ(*res.ret, Value::i64(2)) << "the library's module state kept across RESET";
    host->hooks.on_library = nullptr;
  } else {
    EXPECT_EQ(*res.ret, Value::i64(101)) << "colorsys kept (100), the decimal precision set before RESET not kept (1)";
  }
}

TEST_P(WorkerConformance, ErrorOfAnEarlierRequest) {
  // K12, §10.8: an error object kept from an earlier top-level request and thrown again: sql_error_id
  // 0, the number, SQLSTATE and message copied (rule 2).
  ResultMsg res;
  ASSERT_EQ(prepare(1, "keep_error", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(1, {}, &res,
                       [](const SqlMsg &, SqlResultMsg *r) { sql_error(r, 1146, "42S02", "no such table"); }),
            0u)
      << error_of(res);
  host->hooks.handler = nullptr;
  ASSERT_EQ(prepare(2, "throw_kept_error", {}, kBigint, &res), 0u) << error_of(res);
  ASSERT_EQ(host->call(2, {}, &res), u(Outcome::kGuestError));
  ASSERT_TRUE(res.error);
  EXPECT_EQ(res.error->sql_error_id, 0u);
  EXPECT_EQ(res.error->sql_errno, 1146u);
  EXPECT_EQ(res.error->sqlstate, "42S02");
  EXPECT_NE(res.error->message.find("no such table"), std::string::npos) << res.error->message;
}

TEST_P(WorkerConformance, CloseAfterAnSqlResultTheWorkerCannotHold) {
  // §5.3 (with case 5): after an SQL_RESULT it cannot hold, the worker sends CLOSE for that id.
  const Engine &e = GetParam();
  std::string why, err;
  std::optional<ClassConfig> c = class_of(e.kind, &why);
  ASSERT_TRUE(c) << why;
  c->args.push_back(e.kind == Kind::kJvm ? "-XX:MaxDirectMemorySize=24m" : "--message-mb=16");
  c->name = "small_close";
  LauncherConfig lc;
  lc.launcher_exe = layout().launcher_exe;
  lc.arena_size = wire::kMinArenaSize;
  lc.classes = {*c};
  auto l = Launcher::start(lc, &err);
  ASSERT_NE(l, nullptr) << err;
  auto w = l->acquire("small_close", &err);
  ASSERT_NE(w, nullptr) << err;
  TestHost h(std::move(w));
  ASSERT_TRUE(h.wait_ready(&ready, &err)) << err;
  PrepareMsg p;
  p.routine_id = 1;
  p.language = e.language;
  p.name = "sql_value";
  p.source = kBodies.at("sql_value").of(e.language);
  p.returns = kBigint;
  ResultMsg res;
  ASSERT_EQ(h.prepare(p, &res), 0u) << error_of(res);
  std::vector<std::pair<wire::SqlAction, uint64_t>> seen;
  EXPECT_EQ(h.call(1, {}, &res,
                   [&](const SqlMsg &m, SqlResultMsg *r) {
                     seen.push_back({m.action, m.stmt_id});
                     if (seen.size() == 1)
                       sql_rows(r, {{"v", "", type_of(Tag::kString)}}, {Value::string(std::string(32 << 20, 'a'))});
                     else {
                       *r = SqlResultMsg();
                       r->kind = SqlResultKind::kOk;
                     }
                   }),
            u(Outcome::kGuestError))
      << error_of(res);
  // The Worker may answer the CLOSE itself (CallHooks::sql_answered): what reached the hook is checked.
  ASSERT_GE(seen.size(), 1u);
  if (seen.size() > 1) {
    EXPECT_EQ(seen[1].first, wire::SqlAction::kClose);
    EXPECT_EQ(seen[1].second, seen[0].second);
  }
  EXPECT_TRUE(h.worker.usable());
}

}  // namespace mle_worker_conformance_unittest
