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

/**
  @file components/mle/harness/main.cc

  mle_harness: benchmarks of the MLE worker runtime without mysqld, through
  the same libmle_host the component uses, from an MLE directory (installed
  layout, defaults.h).

    mle_harness --dir <mle dir> --java-home <GraalVM JDK> [--no-aot]
                [--cgroup] <startup|calls|memory|all>

  --cgroup: put workers into cgroup leaves (memory figures, limits); run
  under `systemd-run --user --scope -p Delegate=yes` for that.
*/

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "mle/defaults.h"
#include "mle/launcher.h"
#include "mle/worker.h"

namespace {

using namespace mle;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string dir;
  std::string java_home;
  bool aot = true;
  bool cgroup = false;
  std::string prewarm = "java,js,python,numpy";
  int settle_s = 0;
  std::string command = "all";
};

Options g_opt;
std::string g_cgroup_base;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

[[noreturn]] void die(const std::string &what) {
  fprintf(stderr, "mle_harness: %s\n", what.c_str());
  exit(1);
}

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

/** Moves this process into <own cgroup>/host so that the own cgroup can be
    the base for worker leaves (it must hold no processes). */
std::string prepare_cgroup() {
  std::string line = read_file("/proc/self/cgroup");
  const std::string dir =
      "/sys/fs/cgroup" +
      line.substr(line.find("::") + 2, line.find('\n') - line.find("::") - 2);
  mkdir((dir + "/host").c_str(), 0755);
  std::ofstream(dir + "/host/cgroup.procs") << getpid();
  return dir;
}

Layout layout() {
  Layout l = Layout::installed(g_opt.dir, g_opt.java_home);
  if (!g_opt.aot) l.aot_cache.clear();
  return l;
}

std::unique_ptr<Launcher> start_launcher(std::vector<ClassConfig> classes) {
  LauncherConfig c;
  c.launcher_exe = layout().launcher_exe;
  c.cgroup_base = g_cgroup_base;
  c.classes = std::move(classes);
  c.timeout_ms = 60000;
  std::string err;
  auto l = Launcher::start(c, &err);
  if (!l) die(err);
  for (size_t i = 0; i < l->info().class_errors.size(); i++)
    if (!l->info().class_errors[i].empty()) die(l->info().class_errors[i]);
  return l;
}

std::unique_ptr<Worker> ready_worker(Launcher &l, const std::string &cls,
                                     double *ms = nullptr) {
  const auto t0 = Clock::now();
  std::string err;
  auto p = l.acquire(cls, &err);
  if (!p) die(err);
  auto w = std::make_unique<Worker>(std::move(p));
  ReadyMsg ready;
  if (w->wait_ready(120000, &ready, &err) != Status::kOk) die(err);
  if (ms) *ms = ms_since(t0);
  return w;
}

ParamType type(Tag tag) {
  ParamType t;
  t.tag = tag;
  return t;
}

class Hooks : public CallHooks {
 public:
  std::function<void(const SqlMsg &, SqlResultMsg *)> on_sql;
  void sql(const SqlMsg &m, SqlResultMsg *r) override {
    if (on_sql)
      on_sql(m, r);
    else
      r->kind = SqlResultKind::kOk;
  }
};

void prepare(Worker &w, uint64_t id, const std::string &lang,
             const std::string &entry, const std::string &src,
             std::vector<ParamType> params, std::optional<ParamType> ret) {
  PrepareMsg p;
  p.routine_id = id;
  p.language = lang;
  p.name = "r" + std::to_string(id);
  p.entry = entry;
  p.source = src;
  for (size_t i = 0; i < params.size(); i++)
    p.params.push_back(
        {"p" + std::to_string(i + 1), wire::ParamMode::kIn, params[i]});
  p.returns = ret;
  Hooks hooks;
  ResultMsg res;
  std::string err;
  if (w.prepare(p, hooks, &res, &err) != Status::kOk)
    die("PREPARE " + lang + ": " + (res.error ? res.error->message : err));
}

struct Percentiles {
  double p50, p90, p99;
};

Percentiles time_calls(Worker &w, uint64_t id, const std::vector<Value> &args,
                       Hooks &hooks, int warmup, int n) {
  ResultMsg res;
  std::string err;
  for (int i = 0; i < warmup; i++)
    if (w.call(id, 0, args, hooks, &res, &err) != Status::kOk)
      die("CALL: " + (res.error ? res.error->message : err));
  std::vector<double> us;
  us.reserve(n);
  for (int i = 0; i < n; i++) {
    const auto t0 = Clock::now();
    w.call(id, 0, args, hooks, &res, &err);
    us.push_back(ms_since(t0) * 1000);
  }
  std::sort(us.begin(), us.end());
  return {us[n / 2], us[n * 9 / 10], us[n * 99 / 100]};
}

// ---------------------------------------------------------------- startup

void cmd_startup() {
  printf("\n== startup (AOT cache %s)\n",
         layout().aot_cache.empty() ? "off" : "on");
  printf("%-28s %12s %12s\n", "pre-warmed", "cold (ms)", "pool hit (ms)");
  for (const char *prewarm :
       {"", "java", "java,js", "java,js,python", "java,js,python,numpy"}) {
    JvmClassOptions cold;
    cold.name = "cold";
    cold.prewarm = prewarm;
    JvmClassOptions pooled = cold;
    pooled.name = "pooled";
    pooled.pool_size = 1;
    auto l = start_launcher(
        {jvm_worker_class(layout(), cold), jvm_worker_class(layout(), pooled)});
    std::vector<double> ms;
    for (int i = 0; i < 3; i++) {
      double t;
      ready_worker(*l, "cold", &t);
      ms.push_back(t);
    }
    std::sort(ms.begin(), ms.end());
    // Pool: wait until the pooled worker is READY in the pool, then take it.
    std::vector<ClassStats> stats;
    std::string err;
    for (int i = 0; i < 600; i++) {
      l->stats(&stats, &err);
      if (stats.size() == 2 && stats[1].idle == 1) break;
      usleep(10000);
    }
    usleep(static_cast<useconds_t>(ms[1] * 1000 * 1.5));  // READY inside
    double hit;
    ready_worker(*l, "pooled", &hit);
    printf("%-28s %12.0f %12.2f\n", *prewarm ? prewarm : "(nothing)", ms[1],
           hit);
  }
}

// ---------------------------------------------------------------- calls

void cmd_calls() {
  printf("\n== calls (us; warm: 150k calls first)\n");
  JvmClassOptions o;
  o.prewarm = g_opt.prewarm;
  auto l = start_launcher({jvm_worker_class(layout(), o)});
  auto w = ready_worker(*l, "jvm");
  // Let background compilation after the start finish (see the W7 note).
  if (g_opt.settle_s > 0) sleep(static_cast<unsigned>(g_opt.settle_s));
  Hooks hooks;
  const std::vector<ParamType> p3 = {type(Tag::kI64), type(Tag::kString),
                                     type(Tag::kF64)};
  prepare(*w, 1, "java", "C.f",
          "public class C { public static long f(long a, String b, double c) "
          "{ return a + b.length() + (long) c; } }",
          p3, type(Tag::kI64));
  prepare(*w, 2, "js", "f", "function f(a, b, c) { return a + b.length + c; }",
          p3, type(Tag::kI64));
  prepare(*w, 3, "python", "f",
          "def f(a, b, c):\n    return a + len(b) + int(c)\n", p3,
          type(Tag::kI64));
  prepare(*w, 4, "java", "V.f",
          "public class V { public static double f(float[] v) { double s = 0; "
          "for (float x : v) s += x; return s; } }",
          {type(Tag::kVector)}, type(Tag::kF64));
  prepare(*w, 5, "js", "f",
          "function f(v) { let s = 0; for (let i = 0; i < v.length; i++) s += "
          "v[i]; return s; }",
          {type(Tag::kVector)}, type(Tag::kF64));
  prepare(*w, 6, "python", "f", "def f(v):\n    return float(v.sum())\n",
          {type(Tag::kVector)}, type(Tag::kF64));
  prepare(*w, 10, "python", "f", "def f(v):\n    return 0.0\n",
          {type(Tag::kVector)}, type(Tag::kF64));
  prepare(*w, 7, "java", "S.f",
          "public class S { public static long f() { "
          "com.percona.mle.api.Mle.session().runSql(\"DO 1\"); return 1; } }",
          {}, type(Tag::kI64));
  prepare(*w, 8, "java", "O.f",
          "public class O { public static long f() { return (Long) "
          "com.percona.mle.api.Mle.session().runSql(\"SELECT inner()\")"
          ".fetchOne().get(0); "
          "} }",
          {}, type(Tag::kI64));
  prepare(*w, 9, "java", "I.f",
          "public class I { public static long f() { return 7; } }", {},
          type(Tag::kI64));
  // CREATE ... AS $$ body $$ (empty entry; P4-W3).
  prepare(*w, 11, "js", "", "return p1 + p2.length + p3;", p3, type(Tag::kI64));
  prepare(*w, 12, "python", "", "return p1 + len(p2) + int(p3)", p3,
          type(Tag::kI64));
  prepare(*w, 13, "java", "", "return p1 + p2.length() + p3.longValue();", p3,
          type(Tag::kI64));

  const std::vector<Value> a3 = {Value::i64(1), Value::string("hello"),
                                 Value::f64(2.0)};
  std::vector<float> v768(768, 0.5f);
  const std::vector<Value> av = {Value::vector(v768)};
  struct Case {
    const char *what;
    uint64_t id;
    const std::vector<Value> *args;
  };
  const std::vector<Value> none;
  Hooks nested;
  nested.on_sql = [&](const SqlMsg &, SqlResultMsg *r) {
    ResultMsg inner;
    std::string err;
    Hooks h;
    w->call(9, 0, {}, h, &inner, &err);
    r->kind = SqlResultKind::kRows;
    r->columns.clear();
    r->columns.push_back({"c", "", type(Tag::kI64)});
    r->rows.clear();
    r->rows.push_back(*inner.ret);
  };
  printf("%-40s %8s %8s %8s\n", "call", "p50", "p90", "p99");
  for (const Case &c :
       std::vector<Case>{{"Java, 3 typed args", 1, &a3},
                         {"JS, 3 typed args", 2, &a3},
                         {"Python, 3 typed args", 3, &a3},
                         {"JS body (Oracle form), 3 typed args", 11, &a3},
                         {"Python body, 3 typed args", 12, &a3},
                         {"Java body, 3 typed args", 13, &a3},
                         {"Java, VECTOR(768) as float[]", 4, &av},
                         {"JS, VECTOR(768) as Float32Array", 5, &av},
                         {"Python, VECTOR(768) as numpy array", 6, &av},
                         {"Python, VECTOR(768) unused (conversion)", 10, &av},
                         {"Java + 1 SQL request", 7, &none}}) {
    const Percentiles p = time_calls(*w, c.id, *c.args, hooks, 150000, 50000);
    printf("%-40s %8.2f %8.2f %8.2f\n", c.what, p.p50, p.p90, p.p99);
  }
  const Percentiles p = time_calls(*w, 8, none, nested, 150000, 50000);
  printf("%-40s %8.2f %8.2f %8.2f\n", "Java -> SQL -> nested Java call", p.p50,
         p.p90, p.p99);
}

// ---------------------------------------------------------------- memory

std::string status_field(pid_t pid, const char *name) {
  std::istringstream in(read_file("/proc/" + std::to_string(pid) + "/status"));
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(name, 0) == 0) {
      std::istringstream f(line.substr(strlen(name) + 1));
      long kb;
      f >> kb;
      return std::to_string(kb / 1024);
    }
  return "?";
}

std::string cgroup_mb(uint64_t id, const char *file) {
  if (g_cgroup_base.empty()) return "-";
  const std::string v =
      read_file(g_cgroup_base + "/mle-w" + std::to_string(id) + "/" + file);
  return v.empty() ? "?" : std::to_string(std::stoull(v) >> 20);
}

void cmd_memory() {
  printf("\n== memory per worker (MB; cgroup %s)\n",
         g_cgroup_base.empty() ? "off: run with --cgroup under systemd-run"
                               : "on, memory.max 2048");
  JvmClassOptions o;
  o.prewarm = "";
  auto l = start_launcher({jvm_worker_class(layout(), o)});
  auto w = ready_worker(*l, "jvm");
  const pid_t pid = w->process().pid();
  const uint64_t id = w->process().id();
  Hooks hooks;
  auto report = [&](const char *state) {
    usleep(300000);
    printf("%-34s RSS %6s  cgroup current %6s  peak %6s  heap used %4llu\n",
           state, status_field(pid, "VmRSS:").c_str(),
           cgroup_mb(id, "memory.current").c_str(),
           cgroup_mb(id, "memory.peak").c_str(),
           static_cast<unsigned long long>(w->stats().heap_used >> 20));
  };
  report("idle (nothing loaded)");
  prepare(*w, 1, "java", "J.f",
          "public class J { public static long f() { return 1; } }", {},
          type(Tag::kI64));
  report("+ a Java routine (javac)");
  prepare(*w, 2, "js", "f", "function f() { return 1; }", {}, type(Tag::kI64));
  report("+ a JS routine");
  prepare(*w, 3, "python", "f", "def f():\n    return 1\n", {},
          type(Tag::kI64));
  report("+ a Python routine");
  prepare(*w, 4, "python", "f",
          "import numpy as np\ndef f():\n    return int(np.ones(768) @ "
          "np.ones(768))\n",
          {}, type(Tag::kI64));
  ResultMsg res;
  std::string err;
  w->call(4, 0, {}, hooks, &res, &err);
  report("+ numpy");
  // Workload: 100k calls in each language.
  for (uint64_t id2 : {1, 2, 3, 4})
    for (int i = 0; i < 100000; i++) w->call(id2, 0, {}, hooks, &res, &err);
  report("after 100k calls in each");
}

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--dir" && i + 1 < argc)
      g_opt.dir = argv[++i];
    else if (a == "--java-home" && i + 1 < argc)
      g_opt.java_home = argv[++i];
    else if (a == "--no-aot")
      g_opt.aot = false;
    else if (a == "--cgroup")
      g_opt.cgroup = true;
    else if (a == "--prewarm" && i + 1 < argc)
      g_opt.prewarm = argv[++i];
    else if (a == "--settle" && i + 1 < argc)
      g_opt.settle_s = atoi(argv[++i]);
    else
      g_opt.command = a;
  }
  if (g_opt.dir.empty() || g_opt.java_home.empty())
    die("usage: mle_harness --dir <mle dir> --java-home <jdk> [--no-aot] "
        "[--cgroup] <startup|calls|memory|all>");
  if (g_opt.cgroup) g_cgroup_base = prepare_cgroup();
  const Layout l = layout();
  printf("MLE dir %s; AOT cache %s; venv %s\n", g_opt.dir.c_str(),
         l.aot_cache.empty() ? "no" : "yes",
         l.python_venv.empty() ? "no" : "yes");
  if (g_opt.command == "startup" || g_opt.command == "all") cmd_startup();
  if (g_opt.command == "calls" || g_opt.command == "all") cmd_calls();
  if (g_opt.command == "memory" || g_opt.command == "all") cmd_memory();
  return 0;
}
