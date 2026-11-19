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

#include "worker.hpp"

#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <regex>

#include "api_js.h"
#include "sql_js.h"

namespace mle::v8w {

namespace w = mle::wire;
using common::Interrupts;
using common::Mailbox;
using common::Message;
using v8::Local;
using v8::Value;

namespace {

/** Name of the wrapper function around a body's arrow function (JsOracle.WRAPPER): its frames are
    not the routine's. */
constexpr const char *kWrapper = "$mle$routine";
/** Console output kept per stream until the RESULT (Console.java's LIMIT). */
constexpr size_t kConsoleLimit = 1 << 20;

bool identifier(const std::string &s) {
  static const std::regex re("[A-Za-z_$][A-Za-z0-9_$]*");
  return std::regex_match(s, re);
}

/** A JavaScript string literal (JsOracle.jsString). */
std::string js_string(const std::string &s) {
  std::string b = "\"";
  for (const unsigned char c : s) {
    if (c == '\\' || c == '"') {
      b += '\\';
      b += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      char u[8];
      std::snprintf(u, sizeof u, "\\u%04x", c);
      b += u;
    } else {
      b += static_cast<char>(c);
    }
  }
  return b + "\"";
}

std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t nl = s.find('\n', start);
    out.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

/** A frame's source line as the protocol carries it (Dispatcher.Frame.line: trimmed, at most 200
    characters). */
std::string frame_line(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  s = s.substr(i);
  if (s.size() > 200) s = s.substr(0, 200);
  return s;
}

void native_write(const v8::FunctionCallbackInfo<Value> &info) {
  if (info.Length() < 2) return;
  const int stream = info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1);
  Worker::current()->write(stream, utf8(info.GetIsolate(), info[1]));
}

void native_clear(const v8::FunctionCallbackInfo<Value> &) { Worker::current()->clear_console(); }

void native_sql(const v8::FunctionCallbackInfo<Value> &info) {
  Local<Value> v;
  if (Worker::current()->sql(info).ToLocal(&v)) info.GetReturnValue().Set(v);
}

void native_state(const v8::FunctionCallbackInfo<Value> &info) {
  info.GetReturnValue().Set(Worker::current()->state());
}

void native_nap(const v8::FunctionCallbackInfo<Value> &info) {
  const double ms = info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
  if (ms > 0) usleep(static_cast<useconds_t>(std::min(ms, 10.0) * 1000));
}

void native_library(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = info.GetIsolate();
  Local<Value> ns;
  if (Worker::current()->library_namespace(utf8(iso, info[0]), utf8(iso, info[1])).ToLocal(&ns))
    info.GetReturnValue().Set(ns);
}

void native_read_file(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = info.GetIsolate();
  const std::optional<std::string> text = Worker::current()->package_file(utf8(iso, info[0]));
  if (text) info.GetReturnValue().Set(v8_string(iso, *text));
  else info.GetReturnValue().SetNull();
}

void native_is_dir(const v8::FunctionCallbackInfo<Value> &info) {
  info.GetReturnValue().Set(Worker::current()->package_dir_exists(utf8(info.GetIsolate(), info[0])));
}

/** N.compileFunction(text, filename, params): a CommonJS module's function (line numbers as in the file). */
void native_compile_function(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = info.GetIsolate();
  const Local<v8::Context> ctx = iso->GetCurrentContext();
  v8::ScriptOrigin origin(info[1]);
  v8::ScriptCompiler::Source src(info[0]->ToString(ctx).ToLocalChecked(), origin);
  std::vector<Local<v8::String>> params;
  if (info[2]->IsArray()) {
    const Local<v8::Array> a = info[2].As<v8::Array>();
    for (uint32_t i = 0; i < a->Length(); i++) params.push_back(a->Get(ctx, i).ToLocalChecked().As<v8::String>());
  }
  Local<v8::Function> f;
  if (v8::ScriptCompiler::CompileFunction(ctx, &src, params.size(), params.data(), 0, nullptr).ToLocal(&f))
    info.GetReturnValue().Set(f);
}

}  // namespace

// ------------------------------------------------------------ start

namespace {

/** ICU's data for a libnode with ICU of its own (the RHEL family's nodejs-libs: small data built
    in, the full data in /usr/share/node-<major>/icudata, package nodejs-full-i18n). Node points ICU
    there when it starts; an embedder must too, or Intl (and V8's date formatting) stops the
    process with a fatal error. A libnode that uses the system's ICU (Debian, Ubuntu) needs none. */
void setup_icu(const std::vector<std::string> &dirs) {
#ifdef MLE_LIBNODE_SONAME
  void *lib = dlopen(MLE_LIBNODE_SONAME, RTLD_NOW | RTLD_NOLOAD);
  if (lib == nullptr) return;
  for (int v = 99; v >= 60; v--) {
    void *set_dir = dlsym(lib, ("u_setDataDirectory_" + std::to_string(v)).c_str());
    if (set_dir == nullptr) continue;
    // Only ICU in libnode itself (dladdr names the object that defines the symbol).
    Dl_info where{};
    if (dladdr(set_dir, &where) == 0 || where.dli_fname == nullptr ||
        std::string_view(where.dli_fname).find("libnode") == std::string_view::npos)
      return;
    // The directory with this ICU's data file (the host found the candidates: the worker cannot
    // list /usr/share in its sandbox).
    const std::string file = "icudt" + std::to_string(v) + "l.dat";
    for (const std::string &dir : dirs)
      if (access((dir + "/" + file).c_str(), R_OK) == 0) {
        reinterpret_cast<void (*)(const char *)>(set_dir)(dir.c_str());
        return;
      }
    std::fprintf(stderr, "mle_v8_worker: no %s (ICU %d's data; the package nodejs-full-i18n): Intl will fail\n",
                 file.c_str(), v);
    return;
  }
#else
  (void)dirs;
#endif
}

}  // namespace

Worker::Worker(const Config &config) : m_config(config) {
  s_current = this;
  setup_icu(m_config.icu_data);
  // Few background threads: a worker serves one session (compilation and GC helpers).
  m_platform = v8::platform::NewDefaultPlatform(2);
  v8::V8::InitializeICU();
  v8::V8::InitializePlatform(m_platform.get());
  v8::V8::Initialize();
  m_allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = m_allocator.get();
  // mle.v8_heap_max is the whole heap. V8 sizes the young generation from it (12 MB of 512 MB);
  // V8's own default (Node, js_lang) is 48 MB (16 MB semi-spaces): allocation-heavy code is faster
  // with it (JSON of 200k objects: -21 %, for +22 MB PSS after it; idle the same; exp note 63 §9).
  // Young: 48 MB, at most an eighth of the heap.
  params.constraints.ConfigureDefaultsFromHeapSize(0, m_config.heap_max);
  if (const size_t young = std::min<size_t>(48ULL << 20, m_config.heap_max / 8);
      young > params.constraints.max_young_generation_size_in_bytes()) {
    params.constraints.set_max_young_generation_size_in_bytes(young);
    params.constraints.set_max_old_generation_size_in_bytes(m_config.heap_max - young);
  }
  m_isolate = v8::Isolate::New(params);
  m_isolate->Enter();
  m_isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  m_isolate->SetCaptureStackTraceForUncaughtExceptions(true, w::kMaxStackFrames, v8::StackTrace::kDetailed);
  m_isolate->AddNearHeapLimitCallback(near_heap_limit, this);
  m_isolate->SetHostImportModuleDynamicallyCallback(dynamic_import);

  v8::HandleScope scope(m_isolate);
  const Local<v8::Context> ctx = v8::Context::New(m_isolate);
  ctx->Enter();  // for the process's life
  m_context.Reset(m_isolate, ctx);

  const Local<v8::Object> natives = v8::Object::New(m_isolate);
  const auto fn = [&](const char *name, v8::FunctionCallback cb) {
    (void)natives->Set(ctx, v8_string(m_isolate, name),
                       v8::Function::New(ctx, cb).ToLocalChecked());
  };
  fn("write", native_write);
  fn("clear", native_clear);
  fn("library", native_library);
  fn("sql", native_sql);
  fn("state", native_state);
  fn("nap", native_nap);
  fn("readFile", native_read_file);
  fn("isDir", native_is_dir);
  fn("compileFunction", native_compile_function);
  (void)natives->Set(ctx, v8_string(m_isolate, "packages"), v8_string(m_isolate, m_config.packages));
  v8::TryCatch tc(m_isolate);
  const Local<v8::Script> api =
      v8::Script::Compile(ctx, v8_string(m_isolate, kApiJs)).ToLocalChecked();
  const Local<v8::Function> install = api->Run(ctx).ToLocalChecked().As<v8::Function>();
  Local<Value> arg = natives;
  const Local<v8::Object> helpers =
      install->Call(ctx, v8::Undefined(m_isolate), 1, &arg).ToLocalChecked().As<v8::Object>();
  m_api.Reset(m_isolate, helpers);
  m_show.Reset(m_isolate,
               helpers->Get(ctx, v8_string(m_isolate, "show")).ToLocalChecked().As<v8::Function>());
  m_values = std::make_unique<Values>(m_isolate, ctx, &m_options);
  // The SQL API (sql.js): the globals session, mysql, SqlError; its end-of-call hook.
  const Local<v8::Function> sql_install =
      v8::Script::Compile(ctx, v8_string(m_isolate, kSqlJs)).ToLocalChecked()->Run(ctx).ToLocalChecked().As<v8::Function>();
  const Local<v8::Object> sql_hooks =
      sql_install->Call(ctx, v8::Undefined(m_isolate), 1, &arg).ToLocalChecked().As<v8::Object>();
  m_call_ended.Reset(m_isolate, sql_hooks->Get(ctx, v8_string(m_isolate, "callEnded")).ToLocalChecked().As<v8::Function>());
}

Worker::~Worker() = default;

size_t Worker::near_heap_limit(void *data, size_t current, size_t) {
  // Out of heap: stop the routine (an error that retires the worker), with room to unwind.
  auto *self = static_cast<Worker *>(data);
  self->m_exhausted = true;
  self->m_tainted = true;
  self->m_isolate->TerminateExecution();
  return current + (64U << 20);
}

void Worker::run() {
  common::EngineHooks hooks;
  hooks.interrupt = [](uint64_t) { Worker::current()->terminate(); };
  hooks.interrupted_end = [] { Worker::current()->cancel_terminate(); };
  const auto t0 = std::chrono::steady_clock::now();
  Mailbox::open(m_config.spin, hooks, "mle_v8_worker");
  ready(uint32_t(w::Status::kOk),
        "ready in " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                               .count()) +
            " ms");
  std::vector<std::byte> assembled;
  while (true) {
    const Message m = Mailbox::receive([&assembled](size_t total) {
      assembled.resize(total);
      return assembled.data();
    });
    handle(m);
  }
}

void Worker::ready(uint32_t status, const std::string &info) {
  Writer wr(Mailbox::arena());
  const std::string version = std::string("V8 ") + v8::V8::GetVersion();
  if (!(wr.put<uint32_t>(w::kVersion) && wr.str(m_config.cls) && wr.put<uint32_t>(1) && wr.str("js") &&
        wr.str(version) && wr.str(info)))
    _exit(1);
  stats(uint32_t(w::WorkerState::kIdle));
  Mailbox::post(uint32_t(w::Op::kReady), status, 0, 0, uint32_t(wr.size()));
}

void Worker::stats(uint32_t state) {
  // The heap's figures now and then (they cost a walk over the spaces): at idle, and every 64th call.
  if (state == uint32_t(w::WorkerState::kIdle) && (m_calls++ % 64) == 0) {
    v8::HeapStatistics hs;
    m_isolate->GetHeapStatistics(&hs);
    Mailbox::stats(state, m_tainted, hs.used_heap_size(), m_config.heap_max);
    return;
  }
  Mailbox::stats(state, m_tainted);
}

// ------------------------------------------------------------ requests

void Worker::drop_routine(uint64_t id) {
  const auto it = m_routines.find(id);
  if (it == m_routines.end()) return;
  m_scripts.erase(it->second.script_id);  // its frames' source lines
  m_routines.erase(it);
}

void Worker::handle(const Message &m) {
  v8::HandleScope scope(m_isolate);
  const uint32_t saved_depth = m_depth;
  const uint64_t saved_seq = m_seq;
  const uint64_t saved_scope = m_scope;
  m_scope = ++m_scopes;
  m_depth = m.depth;
  if (m.depth == 0) {
    m_seq = m.call_seq;
    m_lib_open.clear();  // a top-level request starts (LibraryFileSystem.requestStart)
  }
  try {
    switch (static_cast<w::Op>(m.op)) {
      case w::Op::kPrepare:
        prepare(m);
        break;
      case w::Op::kCall:
        call(m);
        break;
      case w::Op::kDrop: {
        Reader r(m.body, 0);
        const auto id = r.get<uint64_t>();
        drop_routine(id);
        m_schemas.erase(id);
        result(uint32_t(w::Status::kOk), nullptr);
        break;
      }
      case w::Op::kReset:
        m_routines.erase(m_routines.begin(), m_routines.end());
        m_scripts.clear();
        m_schemas.clear();
        m_isolate->LowMemoryNotification();  // a reset worker waits for reuse: give memory back
        result(uint32_t(w::Status::kOk), nullptr);
        break;
      case w::Op::kOptions:
        options(m);
        break;
      case w::Op::kParseLibrary:
        parse_library(m);
        break;
      default: {
        ErrorInfo e{"", "unexpected message " + std::to_string(m.op), {}, 0, ""};
        result(uint32_t(w::Status::kInternal), &e);
      }
    }
  } catch (const Malformed &e) {
    std::fprintf(stderr, "mle_v8_worker: fatal: malformed message: %s\n", e.what);
    _exit(1);
  }
  m_depth = saved_depth;
  m_seq = saved_seq;
  m_scope = saved_scope;
}

void Worker::options(const Message &m) {
  Reader r(m.body, 0);
  const auto integer_type = r.get<uint8_t>();
  const auto decimal_type = r.get<uint8_t>();
  const std::string zone(r.str());
  if (integer_type > w::kMaxIntegerType || decimal_type > w::kMaxDecimalType) throw Malformed{"bad options"};
  Options o = m_options;
  if (!o.set_zone(zone)) {
    ErrorInfo e{"MLE-TimeZone", "unknown time zone '" + zone + "'", {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
    return;
  }
  o.integer_type = static_cast<w::IntegerType>(integer_type);
  o.decimal_type = static_cast<w::DecimalType>(decimal_type);
  m_options = o;
  result(uint32_t(w::Status::kOk), nullptr);
}

void Worker::parse_library(const Message &m) {
  Reader r(m.body, 0);
  const std::string language(r.str()), name(r.str()), body(r.str());
  if (language != "js") {
    ErrorInfo e{language, "libraries in " + language + " are not supported", {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
    return;
  }
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  v8::TryCatch tc(m_isolate);
  v8::ScriptOrigin origin(v8_string(m_isolate, name), 0, 0, false, -1, Local<Value>(), false, false, true);
  v8::ScriptCompiler::Source src(v8_string(m_isolate, body), origin);
  if (v8::ScriptCompiler::CompileModule(m_isolate, &src).IsEmpty()) {
    // Oracle's form: "SyntaxError: name:line:col message" (the first line).
    std::string message = compile_error(tc.Message(), name, 0);
    message = message.substr(0, message.find('\n'));
    ErrorInfo e{"js", message, {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
    return;
  }
  (void)ctx;
  result(uint32_t(w::Status::kOk), nullptr);
}

void Worker::prepare(const Message &m) {
  Reader r(m.body, 0);
  const auto id = r.get<uint64_t>();
  const std::string language(r.str()), name(r.str()), entry(r.str()), source(r.str());
  const auto n = r.get<uint32_t>();
  if (n > w::kMaxValues) throw Malformed{"too many parameters"};
  Routine routine;
  std::vector<std::string> names;
  std::vector<uint8_t> modes;
  for (uint32_t i = 0; i < n; i++) {
    names.emplace_back(r.str());
    modes.push_back(r.get<uint8_t>());
    routine.params.push_back(ParamType::read(r));
    if (modes.back() != uint8_t(w::ParamMode::kIn)) routine.outs.push_back(i);
  }
  routine.has_return = r.get<uint8_t>() != 0;
  if (routine.has_return) routine.ret = ParamType::read(r);
  const auto nimports = r.get<uint32_t>();
  if (nimports > w::kMaxValues) throw Malformed{"too many imports"};
  struct Import {
    std::string schema, name, alias;
  };
  std::vector<Import> imports;
  for (uint32_t i = 0; i < nimports; i++) {
    Import imp;
    imp.schema = r.str();
    imp.name = r.str();
    imp.alias = r.str();
    imports.push_back(std::move(imp));
  }
  m_schemas[id] = std::string(r.str());
  (void)r.get<uint8_t>();  // check (names never defined: Python's)
  if (language != "js") {
    ErrorInfo e{language, "the language " + language + " is not available in this worker", {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
    return;
  }
  // A routine is (re)loaded: library DDL may have happened (LibraryFileSystem.revalidate).
  libraries_revalidate();
  std::vector<std::string> usings;
  for (const Import &imp : imports)
    usings.push_back("const " + (imp.alias.empty() ? imp.name : imp.alias) + "=$c.lib(" + js_string(imp.schema) + "," +
                     js_string(imp.name) + ");");
  stats(uint32_t(w::WorkerState::kCompiling));
  const bool top = m_depth == 0;
  if (top) Interrupts::begin(m_seq);
  drop_routine(id);  // prepared again after ALTER
  std::string compile_err;
  v8::TryCatch tc(m_isolate);
  const uint64_t outer = std::exchange(m_routine, id);
  compile(id, name.empty() ? "routine_" + std::to_string(id) : name, entry, source, routine, names, modes, usings,
          &compile_err);
  m_routine = outer;
  const bool interrupted = top && Interrupts::end();
  if (!compile_err.empty()) {
    ErrorInfo e{"js", compile_err, {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
  } else if (tc.HasCaught() || tc.HasTerminated() || routine.fn.IsEmpty()) {
    failed(tc, interrupted);
  } else {
    m_routines.emplace(id, std::move(routine));
    result(uint32_t(w::Status::kOk), nullptr);
  }
}

v8::MaybeLocal<Value> Worker::run_script(const std::string &text, const std::string &name, int prefix,
                                         int *script_id) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  v8::ScriptOrigin origin(v8_string(m_isolate, name));
  Local<v8::Script> script;
  if (!v8::Script::Compile(ctx, v8_string(m_isolate, text), &origin).ToLocal(&script)) return {};
  const int sid = script->GetUnboundScript()->GetId();
  Script s;
  s.name = name;
  s.prefix = prefix;
  s.lines = split_lines(text);
  if (!s.lines.empty() && prefix > 0 && static_cast<size_t>(prefix) <= s.lines[0].size())
    s.lines[0] = s.lines[0].substr(prefix);
  m_scripts[sid] = std::move(s);
  if (script_id != nullptr) *script_id = sid;
  return script->Run(ctx);
}

void Worker::compile(uint64_t, const std::string &name, const std::string &entry, const std::string &source,
                     Routine &r, const std::vector<std::string> &names, const std::vector<uint8_t> &modes,
                     const std::vector<std::string> &usings, std::string *error) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  v8::TryCatch tc(m_isolate);
  tc.SetVerbose(false);
  if (entry == "<eval>") {
    // mle_eval (exp note 39): a strict-mode block; its completion value as text. Syntax errors of
    // the code as written first.
    {
      v8::TryCatch check(m_isolate);
      v8::ScriptOrigin origin(v8_string(m_isolate, "mle_eval"));
      if (v8::Script::Compile(ctx, v8_string(m_isolate, source), &origin).IsEmpty()) {
        *error = compile_error(check.Message(), "mle_eval", 0);
        return;
      }
    }
    const std::string text = "'use strict';{" + source + "\n}";
    r.eval = true;
    // The script is run by call(): keep its text as a function of ours.
    v8::ScriptOrigin origin(v8_string(m_isolate, "mle_eval"));
    Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, v8_string(m_isolate, text), &origin).ToLocal(&script)) {
      *error = compile_error(tc.Message(), "mle_eval", 14);
      return;
    }
    Script s;
    s.name = "mle_eval";
    s.prefix = 14;
    s.lines = split_lines(text);
    if (!s.lines.empty() && s.lines[0].size() >= 14) s.lines[0] = s.lines[0].substr(14);
    r.script_id = script->GetUnboundScript()->GetId();
    m_scripts[r.script_id] = std::move(s);
    // A function that runs the bound script: V8 has no handle to keep a Script, so the routine is
    // a native function holding it.
    r.eval_script = std::make_unique<v8::Global<v8::Script>>(m_isolate, script);
    const Local<v8::External> data = v8::External::New(m_isolate, r.eval_script.get());
    r.fn.Reset(m_isolate, v8::Function::New(ctx,
                                            [](const v8::FunctionCallbackInfo<Value> &info) {
                                              auto *s = static_cast<v8::Global<v8::Script> *>(
                                                  info.Data().As<v8::External>()->Value());
                                              v8::Isolate *iso = info.GetIsolate();
                                              Local<Value> v;
                                              if (s->Get(iso)->Run(iso->GetCurrentContext()).ToLocal(&v))
                                                info.GetReturnValue().Set(v);
                                            },
                                            data)
                                  .ToLocalChecked());
    return;
  }
  std::string text;
  int prefix = 0;
  if (entry.empty()) {
    // Oracle's form (JsOracle.prepare): the body in an arrow function inside a wrapper whose
    // parameters are the routine's; globalThis fresh per call; all on the body's first line.
    for (const std::string &p : names)
      if (!identifier(p)) {
        *error = "'" + p + "' is not a JavaScript identifier";
        return;
      }
    r.async = std::regex_search(source, std::regex("\\bawait\\b"));
    std::string pre = "(function($c){const $G=globalThis;return ";
    if (r.async) pre += "async ";
    pre += std::string("function ") + kWrapper + "(";
    for (size_t i = 0; i < names.size(); i++) pre += (i == 0 ? "$a" : ",$a") + std::to_string(i);
    pre += "){\"use strict\";const globalThis=Object.create($G);";
    for (size_t i = 0; i < names.size(); i++) {
      pre += (i == 0 ? "let " : ",") + names[i] + "=" +
             (modes[i] == uint8_t(w::ParamMode::kOut) ? std::string("null") : "$a" + std::to_string(i));
    }
    if (!names.empty()) pre += ";";
    // USING (Phase 6): each library bound to its name or alias, loaded synchronously.
    for (const std::string &u : usings) pre += u;
    pre += std::string("const $r=") + (r.async ? "await(async($c,$G)=>{" : "(($c,$G)=>{");
    std::string post = "\n";
    if (r.outs.empty()) {
      post += "})();return $r;}})";
    } else {
      post += "})();return[$r";
      for (const uint32_t i : r.outs) post += "," + names[i];
      post += "];}})";
    }
    prefix = static_cast<int>(pre.size());
    text = pre + source + post;
  } else {
    if (!identifier(entry)) {
      *error = "the entry must be a function name, not '" + entry + "'";
      return;
    }
    prefix = static_cast<int>(std::strlen("(function(){"));
    text = "(function(){" + source + "\n;return (typeof " + entry + " === 'function') ? " + entry +
           " : undefined;})()";
  }
  Local<Value> made;
  if (!run_script(text, name, prefix, &r.script_id).ToLocal(&made)) {
    if (tc.HasTerminated()) {
      tc.ReThrow();
      return;
    }
    if (!tc.Message().IsEmpty() && tc.Message()->GetScriptResourceName()->IsString() &&
        tc.Exception()->IsObject() && utf8(m_isolate, tc.Exception()).starts_with("SyntaxError")) {
      *error = compile_error(tc.Message(), name, prefix);
      return;
    }
    tc.ReThrow();
    return;
  }
  if (entry.empty()) {
    // The factory, with the helpers ($c).
    Local<Value> api = m_api.Get(m_isolate);
    if (!made.As<v8::Function>()->Call(ctx, v8::Undefined(m_isolate), 1, &api).ToLocal(&made)) {
      tc.ReThrow();
      return;
    }
  }
  if (!made->IsFunction()) {
    *error = "the source does not define a function " + entry;
    return;
  }
  r.fn.Reset(m_isolate, made.As<v8::Function>());
}

void Worker::call(const Message &m) {
  Reader r(m.body, 0);
  const auto id = r.get<uint64_t>();
  (void)r.get<uint32_t>();  // events
  const auto n = r.get<uint32_t>();
  const auto it = m_routines.find(id);
  if (it == m_routines.end() || it->second.params.size() != n) {
    ErrorInfo e{"", "routine " + std::to_string(id) + " is not prepared with " + std::to_string(n) + " parameters",
                {}, 0, ""};
    result(uint32_t(w::Status::kInternal), &e);
    return;
  }
  const Routine &routine = it->second;
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  const bool top = m_depth == 0;
  v8::TryCatch tc(m_isolate);
  Local<Value> args[64];
  std::vector<Local<Value>> many;
  Local<Value> *argv = args;
  if (n > 64) {
    many.resize(n);
    argv = many.data();
  }
  try {
    for (uint32_t i = 0; i < n; i++) argv[i] = m_values->argument(r, routine.params[i]);
  } catch (const ConversionError &e) {
    ErrorInfo info{"MLE-Type", e.message, {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &info);
    return;
  }
  if (top) Interrupts::begin(m_seq);
  stats(uint32_t(w::WorkerState::kRunning));
  Local<Value> ret;
  const uint64_t outer = std::exchange(m_routine, id);
  const bool ok = routine.fn.Get(m_isolate)->Call(ctx, v8::Undefined(m_isolate), static_cast<int>(n), argv)
                      .ToLocal(&ret);
  m_routine = outer;
  // The SQL API's end of a call (sql.js: a nested call's unread result closed).
  if (!m_call_ended.IsEmpty()) {
    v8::TryCatch inner(m_isolate);
    Local<Value> arg = v8::Boolean::New(m_isolate, top);
    (void)m_call_ended.Get(m_isolate)->Call(ctx, v8::Undefined(m_isolate), 1, &arg);
  }
  bool settled = true;
  if (ok) {
    m_isolate->PerformMicrotaskCheckpoint();
    if (routine.async && ret->IsPromise()) {
      const Local<v8::Promise> p = ret.As<v8::Promise>();
      if (p->State() == v8::Promise::kFulfilled) {
        ret = p->Result();
      } else if (p->State() == v8::Promise::kRejected) {
        m_isolate->ThrowException(p->Result());
        settled = false;
      } else {
        settled = false;
      }
    }
  }
  const bool interrupted = top && Interrupts::end();
  if (m_exhausted) {
    // The heap limit hit after the routine returned (callEnded, the microtasks, whose TryCatch may
    // have taken the termination): the same error as during the call.
    m_exhausted = false;
    m_isolate->CancelTerminateExecution();
    ErrorInfo e{"js", "RangeError: out of memory (the V8 heap limit, mle.v8_heap_max)", {}, 0, ""};
    result(uint32_t(w::Status::kExhausted), &e);
    return;
  }
  if (ok && settled && !tc.HasCaught() && !tc.HasTerminated()) {
    if (interrupted) {
      ErrorInfo e{"", "interrupted", {}, 0, ""};
      result(uint32_t(w::Status::kInterrupted), &e);
      return;
    }
    if (routine.eval) {
      Local<Value> v = ret;
      Local<Value> shown;
      if (!m_show.Get(m_isolate)->Call(ctx, v8::Undefined(m_isolate), 1, &v).ToLocal(&shown)) shown = v8::Null(m_isolate);
      result(uint32_t(w::Status::kOk), nullptr, &routine, shown);
      return;
    }
    if (!routine.outs.empty()) {
      if (!ret->IsArray()) {
        ErrorInfo e{"", "the routine's wrapper did not return its OUT values", {}, 0, ""};
        result(uint32_t(w::Status::kInternal), &e);
        return;
      }
      const Local<v8::Array> all = ret.As<v8::Array>();
      result(uint32_t(w::Status::kOk), nullptr, &routine, all->Get(ctx, 0).ToLocalChecked(), all);
      return;
    }
    result(uint32_t(w::Status::kOk), nullptr, &routine, ret);
    return;
  }
  if (ok && !settled && !tc.HasCaught()) {
    ErrorInfo e{"MLE-Type",
                m_depth > 0 ? "an async JavaScript routine (one that uses await) cannot run while another routine "
                              "of this worker runs (a nested call); its promise did not settle"
                            : "the routine's promise did not settle",
                {}, 0, ""};
    result(uint32_t(w::Status::kGuestError), &e);
    return;
  }
  failed(tc, interrupted);
}

// ------------------------------------------------------------ errors

std::string Worker::compile_error(Local<v8::Message> message, const std::string &name, int prefix) {
  if (message.IsEmpty()) return "SyntaxError";
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  std::string text = utf8(m_isolate, message->Get());
  // "Uncaught SyntaxError: msg" -> the message after the error's name
  if (text.starts_with("Uncaught ")) text = text.substr(9);
  std::string kind = "SyntaxError";
  if (const size_t colon = text.find(": "); colon != std::string::npos && colon < 40) {
    kind = text.substr(0, colon);
    text = text.substr(colon + 2);
  }
  const int line = message->GetLineNumber(ctx).FromMaybe(0);
  int column = message->GetStartColumn(ctx).FromMaybe(0) + 1;
  int end = message->GetEndColumn(ctx).FromMaybe(column);
  std::string source;
  Local<v8::String> src;
  if (message->GetSourceLine(ctx).ToLocal(&src)) source = utf8(m_isolate, src);
  if (line == 1 && prefix > 0) {
    column = std::max(1, column - prefix);
    end = std::max(column, end - prefix);
    if (static_cast<int>(source.size()) >= prefix) source = source.substr(prefix);
  }
  // Oracle's (GraalJS's) form: "SyntaxError: name:line:col message", the line, a caret.
  std::string out = kind + ": " + name + ":" + std::to_string(line) + ":" + std::to_string(column) + " " + text;
  if (!source.empty() && source.size() < 400) {
    out += "\n" + source + "\n" + std::string(static_cast<size_t>(std::max(0, column - 1)), ' ') +
           std::string(static_cast<size_t>(std::max(1, end - (column - 1) - 1)), '^') + "\n";
  }
  return out;
}

ErrorInfo Worker::error_of(Local<Value> exception, Local<v8::Message> message) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  ErrorInfo e;
  e.language = "js";
  e.message = m_values->str(exception);
  // A SqlError (the SQL API's class: marked $mle$sql, with code and sqlState).
  if (exception->IsObject()) {
    const Local<v8::Object> o = exception.As<v8::Object>();
    Local<Value> mark;
    if (o->Get(ctx, v8_string(m_isolate, "$mle$sql")).ToLocal(&mark) && mark->IsTrue()) {
      Local<Value> code, state, text;
      if (o->Get(ctx, v8_string(m_isolate, "message")).ToLocal(&text)) e.message = m_values->str(text);
      if (o->Get(ctx, v8_string(m_isolate, "code")).ToLocal(&code) && code->IsNumber()) {
        // SIGNAL's MYSQL_ERRNO range; anything else (NaN from new SqlError(s, m, 'x')): 1644, SIGNAL's own.
        const double c = code.As<v8::Number>()->Value();
        e.sql_errno = c >= 1 && c <= 65535 && c == std::floor(c) ? static_cast<uint32_t>(c) : 1644;
      }
      if (o->Get(ctx, v8_string(m_isolate, "sqlState")).ToLocal(&state) && state->IsString()) {
        e.sql_state = utf8(m_isolate, state);
        const bool valid = e.sql_state.size() == 5 &&
                           std::all_of(e.sql_state.begin(), e.sql_state.end(),
                                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); });
        if (!valid) {
          e.message += " (SqlError with the invalid SQLSTATE '" + e.sql_state + "')";
          e.sql_state.clear();
          e.sql_errno = 0;
        }
      }
    }
  }
  Local<v8::StackTrace> trace = v8::Exception::GetStackTrace(exception);
  if (trace.IsEmpty() && !message.IsEmpty()) trace = message->GetStackTrace();
  if (!trace.IsEmpty()) {
    for (int i = 0; i < trace->GetFrameCount() && e.frames.size() < w::kMaxStackFrames; i++) {
      const Local<v8::StackFrame> f = trace->GetFrame(m_isolate, static_cast<uint32_t>(i));
      const auto s = m_scripts.find(f->GetScriptId());
      if (s == m_scripts.end()) continue;  // not a routine's (the worker's own JavaScript)
      const std::string function = utf8(m_isolate, f->GetFunctionName());
      if (function == kWrapper) continue;
      ErrorInfo::Frame fr;
      fr.function = function;
      fr.file = s->second.name;
      fr.line = static_cast<uint32_t>(f->GetLineNumber());
      int column = f->GetColumn();
      if (fr.line == 1) column = std::max(1, column - s->second.prefix);
      fr.column = fr.end_column = static_cast<uint32_t>(column);
      if (fr.line >= 1 && fr.line <= s->second.lines.size()) fr.text = frame_line(s->second.lines[fr.line - 1]);
      e.frames.push_back(std::move(fr));
    }
  }
  return e;
}

void Worker::failed(v8::TryCatch &tc, bool interrupted) {
  if (tc.HasTerminated() || (interrupted && !tc.HasCaught())) {
    const bool exhausted = std::exchange(m_exhausted, false);
    m_isolate->CancelTerminateExecution();
    if (exhausted) {
      ErrorInfo e{"js", "RangeError: out of memory (the V8 heap limit, mle.v8_heap_max)", {}, 0, ""};
      result(uint32_t(w::Status::kExhausted), &e);
    } else {
      ErrorInfo e{"js", "interrupted", {}, 0, ""};
      result(uint32_t(w::Status::kInterrupted), &e);
    }
    return;
  }
  if (interrupted) {
    ErrorInfo e{"js", "interrupted", {}, 0, ""};
    result(uint32_t(w::Status::kInterrupted), &e);
    return;
  }
  if (!tc.HasCaught()) {
    ErrorInfo e{"", "the routine failed without an exception", {}, 0, ""};
    result(uint32_t(w::Status::kInternal), &e);
    return;
  }
  ErrorInfo e = error_of(tc.Exception(), tc.Message());
  result(uint32_t(w::Status::kGuestError), &e);
}

// ------------------------------------------------------------ results

void Worker::write(int stream, std::string_view text) {
  std::string &out = stream == 2 ? m_err : m_out;
  const size_t room = out.size() < kConsoleLimit ? kConsoleLimit - out.size() : 0;
  out.append(text.substr(0, room));
}

void Worker::clear_console() {
  m_out.clear();
  m_err.clear();
  m_cleared = true;
}

void Worker::write_result(Writer &wr, const ErrorInfo *error, const Routine *r, Local<Value> ret,
                          Local<v8::Array> outs) {
  const auto need = [](bool ok) {
    if (!ok) throw ArenaFull{};
  };
  if (r != nullptr && error == nullptr && (r->has_return || r->eval)) {
    need(wr.put<uint8_t>(1));
    if (r->eval) {
      ParamType text;
      text.tag = w::Tag::kString;
      m_values->write_as(wr, ret, text);
    } else {
      m_values->write_as(wr, ret, r->ret);
    }
  } else {
    need(wr.put<uint8_t>(0));
  }
  if (error == nullptr && r != nullptr && !r->outs.empty() && !outs.IsEmpty()) {
    const Local<v8::Context> ctx = m_context.Get(m_isolate);
    need(wr.put(static_cast<uint32_t>(r->outs.size())));
    for (size_t k = 0; k < r->outs.size(); k++) {
      const uint32_t i = r->outs[k];
      need(wr.put(i));
      m_values->write_as(wr, outs->Get(ctx, static_cast<uint32_t>(k + 1)).ToLocalChecked(), r->params[i]);
    }
  } else {
    need(wr.put<uint32_t>(0));
  }
  need(wr.str(m_out) && wr.str(m_err) && wr.put<uint8_t>(m_cleared ? 1 : 0));
  if (error == nullptr) {
    need(wr.put<uint8_t>(0));
    return;
  }
  need(wr.put<uint8_t>(1) && wr.str(error->language) && wr.str(error->message) &&
       wr.put(static_cast<uint32_t>(error->frames.size())));
  for (const ErrorInfo::Frame &f : error->frames)
    need(wr.str(f.function) && wr.str(f.file) && wr.put(f.line) && wr.put(f.column) && wr.put(f.end_column) &&
         wr.str(f.text));
  need(wr.put(error->sql_errno) && wr.str(error->sql_state));
}

void Worker::result(uint32_t status, const ErrorInfo *error, const Routine *r, Local<Value> ret,
                    Local<v8::Array> outs) {
  ErrorInfo instead;
  const auto attempt = [&](Writer &wr, const ErrorInfo *e, const Routine *rr) {
    const Converting converting(m_converting);
    write_result(wr, e, rr, ret, outs);
    return wr.size();
  };
  const auto post = [&](const ErrorInfo *e, const Routine *rr) -> bool {
    // In the arena if it fits; else in a buffer of up to the host's limit, sent in chunks.
    {
      Writer wr(Mailbox::arena());
      try {
        const size_t len = attempt(wr, e, rr);
        stats(m_depth == 0 ? uint32_t(w::WorkerState::kIdle) : uint32_t(w::WorkerState::kRunning));
        Mailbox::post(uint32_t(w::Op::kResult), status, m_depth, m_seq, uint32_t(len));
        return true;
      } catch (const ArenaFull &) {
      }
    }
    m_big.resize(w::kMaxWorkerMessage);
    Writer wr(m_big);
    try {
      const size_t len = attempt(wr, e, rr);
      stats(m_depth == 0 ? uint32_t(w::WorkerState::kIdle) : uint32_t(w::WorkerState::kRunning));
      Mailbox::send(uint32_t(w::Op::kResult), status, m_depth, m_seq, std::span(m_big).first(len));
      m_big.clear();
      m_big.shrink_to_fit();
      return true;
    } catch (const ArenaFull &) {
      m_big.clear();
      m_big.shrink_to_fit();
      return false;
    }
  };
  try {
    if (post(error, r)) {
      m_out.clear(), m_err.clear(), m_cleared = false;
      return;
    }
    instead = ErrorInfo{"MLE-Type", "the routine's result with its output is larger than MLE allows (64 MiB)", {}, 0, ""};
  } catch (const ConversionError &c) {
    instead = ErrorInfo{"MLE-Type", c.message, {}, 0, ""};
  }
  status = uint32_t(w::Status::kGuestError);
  if (!post(&instead, nullptr)) {
    m_out.clear(), m_err.clear();  // the output alone is too large: dropped
    instead.message = "the routine's output is larger than MLE allows (64 MiB)";
    (void)post(&instead, nullptr);
  }
  m_out.clear(), m_err.clear(), m_cleared = false;
}

// ------------------------------------------------------------ SQL

Local<Value> Worker::state() {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  const auto schema = m_schemas.find(m_routine);
  Local<Value> items[] = {
      v8::Number::New(m_isolate, m_depth),
      v8::Number::New(m_isolate, static_cast<double>(m_scope)),
      schema == m_schemas.end() ? Local<Value>(v8::Null(m_isolate)) : Local<Value>(v8_string(m_isolate, schema->second)),
      v8::Number::New(m_isolate, static_cast<double>(m_options.integer_type)),
      v8::Number::New(m_isolate, static_cast<double>(m_options.decimal_type)),
      v8::Number::New(m_isolate, static_cast<double>(Mailbox::arena().size())),
      v8::Number::New(m_isolate, static_cast<double>(m_seq)),
  };
  (void)ctx;
  return v8::Array::New(m_isolate, items, std::size(items));
}

namespace {

/** u32 n + n x (u32 level, u32 code, str message) as [[level, code, message], ...]. */
Local<Value> warnings_of(v8::Isolate *iso, Reader &r) {
  const auto n = r.get<uint32_t>();
  if (n > 65535) throw Malformed{"too many warnings"};
  const Local<v8::Array> a = v8::Array::New(iso, static_cast<int>(n));
  const Local<v8::Context> ctx = iso->GetCurrentContext();
  for (uint32_t i = 0; i < n; i++) {
    const auto level = r.get<uint32_t>(), code = r.get<uint32_t>();
    Local<Value> w[] = {v8::Number::New(iso, level), v8::Number::New(iso, code), v8_string(iso, r.str())};
    (void)a->Set(ctx, i, v8::Array::New(iso, w, 3));
  }
  return a;
}

Local<Value> number(v8::Isolate *iso, uint64_t v) {
  return v <= (uint64_t{1} << 53) ? Local<Value>(v8::Number::New(iso, static_cast<double>(v)))
                                   : Local<Value>(v8::BigInt::NewFromUnsigned(iso, v));
}

}  // namespace

v8::MaybeLocal<Value> Worker::sql(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = m_isolate;
  const Local<v8::Context> ctx = m_context.Get(iso);
  const auto u32 = [&](int i) { return info[i]->Uint32Value(ctx).FromMaybe(0); };
  const auto f64 = [&](int i) { return info[i]->NumberValue(ctx).FromMaybe(0); };
  const auto throw_type = [&](const std::string &m) {
    iso->ThrowException(v8::Exception::TypeError(v8_string(iso, m)));
    return v8::MaybeLocal<Value>();
  };
  if (info.Length() < 8) return throw_type("sql: 8 arguments");
  if (m_converting > 0) return throw_type(kNoHostWhileConverting);
  // A KILL that stopped a nested routine (its failure cancels the termination) stops this one too,
  // once back in JavaScript: it could catch the SqlError and run on.
  struct Rearm {
    v8::Isolate *iso;
    ~Rearm() {
      if (Interrupts::interrupted()) iso->TerminateExecution();
    }
  } rearm{iso};
  const auto action = static_cast<uint8_t>(u32(0));
  const auto sid = static_cast<uint64_t>(f64(1));
  const std::string text = info[2]->IsString() ? utf8(iso, info[2]) : std::string();
  const auto flags = static_cast<uint8_t>(u32(4));
  const auto width = u32(5);
  // The statement's integerType and decimalType; -1: the session's.
  Options rules = m_options;
  const auto i32 = [&](int i) { return info[i]->Int32Value(ctx).FromMaybe(-1); };
  if (const int it = i32(6); it >= 0 && it <= w::kMaxIntegerType) rules.integer_type = static_cast<w::IntegerType>(it);
  if (const int dt = i32(7); dt >= 0 && dt <= w::kMaxDecimalType) rules.decimal_type = static_cast<w::DecimalType>(dt);

  // The message: in a buffer the size of the arena, larger when it does not fit (up to the host's limit).
  Local<v8::Array> params;
  uint32_t nparams = 0;
  if (info[3]->IsArray()) {
    params = info[3].As<v8::Array>();
    nparams = params->Length();
  }
  if (nparams > w::kMaxValues)
    return throw_type("Incorrect arguments: " + std::to_string(nparams) + " values in one statement (at most " +
                      std::to_string(w::kMaxValues) + ")");
  size_t size = Mailbox::arena().size();
  size_t len = 0;
  while (true) {
    m_sql_buf.resize(size);
    Writer wr(m_sql_buf);
    const Converting converting(m_converting);
    try {
      bool ok = wr.put(action) && wr.put(sid) && wr.str(text) && wr.put(nparams);
      for (uint32_t i = 0; ok && i < nparams; i++) {
        Local<Value> p;
        if (!params->Get(ctx, i).ToLocal(&p)) return {};
        m_values->write_param(wr, p);
      }
      ok = ok && wr.put(flags) && wr.put(uint32_t{0}) && wr.put(width);
      if (!ok) throw ArenaFull{};
      len = wr.size();
      break;
    } catch (const ArenaFull &) {
      if (size >= w::kMaxWorkerMessage) {
        // MySQL's error for a packet over max_allowed_packet (not its SQLSTATE 08S01): sql.js makes it a SqlError.
        Local<Value> e[] = {v8::Number::New(iso, 0), v8::Number::New(iso, 1153), v8_string(iso, "HY000"),
                            v8_string(iso, "the SQL statement with its parameters is larger than MLE allows (" +
                                               std::to_string(w::kMaxWorkerMessage >> 20) + " MiB)"),
                            v8::Number::New(iso, -1)};
        m_sql_buf.clear();
        m_sql_buf.shrink_to_fit();
        return v8::Array::New(iso, e, 5);
      }
      size = std::min<size_t>(size * 4, w::kMaxWorkerMessage);
    } catch (const ConversionError &c) {
      return throw_type(c.message);
    }
  }
  Mailbox::send(uint32_t(w::Op::kSql), uint32_t(w::Status::kOk), m_depth, m_seq, std::span(m_sql_buf).first(len));
  if (m_sql_buf.size() > (4U << 20)) {
    m_sql_buf.clear();
    m_sql_buf.shrink_to_fit();
  }
  std::vector<std::byte> assembled;
  while (true) {
    const Message m = Mailbox::receive([&assembled](size_t total) {
      assembled.resize(total);
      return assembled.data();
    });
    if (m.op == uint32_t(w::Op::kSqlResult) && m.depth == m_depth) {
      Reader r(m.body, 0);
      switch (static_cast<w::SqlResultKind>(r.get<uint8_t>())) {
        case w::SqlResultKind::kError: {
          const auto code = r.get<uint32_t>();
          const std::string state(r.str()), message(r.str());
          const auto row = r.get<uint32_t>();
          Local<Value> e[] = {v8::Number::New(iso, 0), v8::Number::New(iso, code), v8_string(iso, state),
                              v8_string(iso, message),
                              v8::Number::New(iso, row == w::kNoRow ? -1.0 : static_cast<double>(row))};
          return v8::Array::New(iso, e, 5);
        }
        case w::SqlResultKind::kOk: {
          const auto affected = r.get<uint64_t>(), insert_id = r.get<uint64_t>();
          const Local<Value> warns = warnings_of(iso, r);
          const bool more_results = r.get<uint8_t>() != 0;
          const auto pcount = r.get<uint32_t>();
          const auto exec_us = r.get<uint64_t>();
          Local<Value> o[] = {v8::Number::New(iso, 1),       number(iso, affected),
                              number(iso, insert_id),        warns,
                              v8::Boolean::New(iso, more_results), v8::Number::New(iso, pcount),
                              v8::Number::New(iso, static_cast<double>(exec_us))};
          return v8::Array::New(iso, o, 7);
        }
        case w::SqlResultKind::kRows: {
          const auto ncols = r.get<uint32_t>();
          if (ncols > 4096) throw Malformed{"too many columns"};
          std::vector<std::string> types;
          const Local<v8::Array> labels = v8::Array::New(iso, static_cast<int>(ncols));
          const Local<v8::Array> columns = v8::Array::New(iso, static_cast<int>(ncols));
          for (uint32_t i = 0; i < ncols; i++) {
            const std::string name(r.str()), table(r.str());
            const ParamType t = ParamType::read(r);
            const std::string org_name(r.str()), schema(r.str()), org_table(r.str()), charset(r.str()),
                collation(r.str());
            const auto flags_ = r.get<uint32_t>();
            types.push_back(t.type_name);
            (void)labels->Set(ctx, i, v8_string(iso, name));
            Local<Value> c[] = {v8_string(iso, name),          v8_string(iso, table),
                                v8::Number::New(iso, static_cast<double>(t.tag)), v8::Boolean::New(iso, t.is_unsigned),
                                v8::Number::New(iso, t.length), v8::Number::New(iso, t.precision),
                                v8::Number::New(iso, t.scale),  v8_string(iso, t.type_name),
                                v8_string(iso, org_name),       v8_string(iso, schema),
                                v8_string(iso, org_table),      v8_string(iso, charset),
                                v8_string(iso, collation),      v8::Number::New(iso, flags_)};
            (void)columns->Set(ctx, i, v8::Array::New(iso, c, std::size(c)));
          }
          const auto total = r.get<uint64_t>();
          const auto nrows = r.get<uint32_t>();
          // The rows' values in one flat array, row by row (nrows * ncols): sql.js makes the rows
          // (arrays with the labels as properties, rowsOf), in JavaScript, where V8 compiles their
          // shape once; built here through the API, each property costs a generic lookup.
          if (static_cast<uint64_t>(nrows) * ncols > (1ULL << 27)) throw Malformed{"too many values"};
          std::vector<Local<Value>> values(static_cast<size_t>(nrows) * ncols);
          try {
            for (size_t k = 0; k < values.size(); k++) values[k] = m_values->column(r, types[k % ncols], &rules);
          } catch (const ConversionError &c) {
            return throw_type(c.message);
          }
          const bool more = r.get<uint8_t>() != 0, more_results = r.get<uint8_t>() != 0;
          const Local<Value> warns = warnings_of(iso, r);
          const auto exec_us = r.get<uint64_t>();
          const Local<v8::Array> rows = v8::Array::New(iso, values.data(), values.size());
          Local<Value> o[] = {v8::Number::New(iso, 2),     labels, columns, number(iso, total), rows,
                              v8::Boolean::New(iso, more), v8::Boolean::New(iso, more_results), warns,
                              v8::Number::New(iso, static_cast<double>(exec_us))};
          return v8::Array::New(iso, o, std::size(o));
        }
      }
      throw Malformed{"bad SQL result kind"};
    }
    const bool nested = m.op == uint32_t(w::Op::kCall) || m.op == uint32_t(w::Op::kPrepare) ||
                        m.op == uint32_t(w::Op::kDrop);
    if (nested && m.depth == m_depth + 1) {
      handle(m);
      continue;
    }
    std::fprintf(stderr, "mle_v8_worker: fatal: unexpected message %u at depth %u during SQL\n", m.op, m.depth);
    _exit(1);
  }
}

}  // namespace mle::v8w
