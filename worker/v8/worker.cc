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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
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
/** Console output the worker keeps per stream until a RESULT carries it: the end of the stream. */
constexpr size_t kConsoleLimit = 1 << 20;
/** The language identifier of READY, PREPARE and Error (Appendix B.1). */
constexpr const char *kJs = "js";

/** Cuts text to at most max bytes at a character boundary (§7.4). */
void cut(std::string &s, size_t max) {
  if (s.size() <= max) return;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  s.resize(n);
}

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

/** A frame's source line as the protocol carries it: trimmed, at most 200 bytes. */
std::string frame_line(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  s = s.substr(i);
  cut(s, 200);
  return s;
}

/** The exact UTF-8 of a string argument that is a name; nullopt (and a TypeError thrown) if it is
    not a well-formed string. */
std::optional<std::string> name_argument(v8::Isolate *iso, Local<Value> v, const char *what) {
  std::optional<std::string> s;
  if (v->IsString()) s = utf8_exact(iso, v.As<v8::String>());
  if (!s)
    iso->ThrowException(v8::Exception::TypeError(
        v8_string(iso, std::string(what) + " must be a string of well-formed Unicode (no unpaired surrogate)")));
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

void native_hand_back(const v8::FunctionCallbackInfo<Value> &info) { Worker::current()->hand_back(info); }

void native_state(const v8::FunctionCallbackInfo<Value> &info) {
  info.GetReturnValue().Set(Worker::current()->state());
}

void native_nap(const v8::FunctionCallbackInfo<Value> &info) {
  const double ms = info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
  if (ms > 0) usleep(static_cast<useconds_t>(std::min(ms, 10.0) * 1000));
}

void native_library(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = info.GetIsolate();
  const std::optional<std::string> schema = name_argument(iso, info[0], "a library's schema");
  if (!schema) return;
  const std::optional<std::string> name = name_argument(iso, info[1], "a library's name");
  if (!name) return;
  Local<Value> ns;
  if (Worker::current()->library_namespace(*schema, *name).ToLocal(&ns)) info.GetReturnValue().Set(ns);
}

void native_read_file(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = info.GetIsolate();
  const std::optional<std::string> text = Worker::current()->package_file(utf8(iso, info[0]));
  info.GetReturnValue().SetNull();
  if (!text) return;
  try {
    info.GetReturnValue().Set(v8_string(iso, *text));
  } catch (const ConversionError &) {  // a file larger than a string can be: as if it were not there
  }
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
  Mailbox::stats(uint32_t(w::WorkerState::kStarting), false);
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
  fn("handBack", native_hand_back);
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
  // The SQL API (sql.js): the globals session, mysql, SqlError.
  const Local<v8::Function> sql_install =
      v8::Script::Compile(ctx, v8_string(m_isolate, kSqlJs)).ToLocalChecked()->Run(ctx).ToLocalChecked().As<v8::Function>();
  (void)sql_install->Call(ctx, v8::Undefined(m_isolate), 1, &arg).ToLocalChecked();
  m_probe.Reset(m_isolate, v8::Script::Compile(ctx, v8_string(m_isolate, "(function () {})"))
                               .ToLocalChecked()
                               ->Run(ctx)
                               .ToLocalChecked()
                               .As<v8::Function>());
  m_handed.Reset(m_isolate, v8::Private::New(m_isolate));
}

Worker::~Worker() = default;

size_t Worker::near_heap_limit(void *data, size_t current, size_t) {
  // Out of heap: stop the guest code (EXHAUSTED, and the worker is retired), with room to unwind.
  auto *self = static_cast<Worker *>(data);
  self->m_exhausted = true;
  self->m_tainted = true;
  self->m_isolate->TerminateExecution();
  return current + (64U << 20);
}

void Worker::run() {
  const std::string runtime = std::string("V8 ") + v8::V8::GetVersion();
  const std::string info =
      "class " + m_config.cls + ", ready in " +
      std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count()) +
      " ms";
  const std::string_view languages[] = {kJs};
  stats(uint32_t(w::WorkerState::kIdle));
  Mailbox::ready(info, "v8", languages, runtime);
  std::vector<std::byte> assembled;
  while (true) {
    const Message m = Mailbox::receive([&](size_t total) { return allocate(assembled, total); });
    handle(m);
    if (assembled.capacity() > (4U << 20)) std::vector<std::byte>().swap(assembled);
  }
}

std::byte *Worker::allocate(std::vector<std::byte> &buffer, size_t total) {
  if (m_config.message_max != 0 && total > m_config.message_max) return nullptr;
  try {
    buffer.resize(total);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
  return buffer.data();
}

void Worker::stats(uint32_t state) {
  // The heap's figures now and then (they cost a walk over the spaces): at idle, and every 64th call.
  if (state == uint32_t(w::WorkerState::kIdle) && (m_calls++ % 64) == 0) {
    v8::HeapStatistics hs;
    m_isolate->GetHeapStatistics(&hs);
    Mailbox::stats(state, m_tainted, std::min<uint64_t>(hs.used_heap_size(), m_config.heap_max), m_config.heap_max);
    return;
  }
  Mailbox::stats(state, m_tainted);
}

// ------------------------------------------------------------ requests

void Worker::drop_routine(uint64_t id) {
  m_routines.erase(id);  // a running activation keeps the code (§8.3)
  m_answers.erase(id);   // and the library answers of the id are discarded (§9.10)
}

const char *Worker::host_unavailable() const {
  if (m_converting > 0)
    return "SQL and library imports are not available in toString or toJSON while MLE converts a value";
  if (m_request == nullptr || !m_request->guest) return "SQL and library imports are available only while a routine runs";
  return nullptr;
}

bool Worker::termination_pending() {
  v8::TryCatch tc(m_isolate);
  // Entering JavaScript acts on a termination that was requested and not acted on yet.
  return m_probe.Get(m_isolate)->Call(m_context.Get(m_isolate), v8::Undefined(m_isolate), 0, nullptr).IsEmpty() &&
         tc.HasTerminated();
}

void Worker::discard_microtasks() {
  // V8 empties the queue when a termination stops it; a job that enters JavaScript is stopped at
  // once, so no guest code runs here.
  m_isolate->TerminateExecution();
  {
    v8::TryCatch tc(m_isolate);
    m_isolate->PerformMicrotaskCheckpoint();
  }
  m_isolate->CancelTerminateExecution();
}

void Worker::handle(const Message &m) {
  v8::HandleScope scope(m_isolate);
  const auto op = static_cast<w::Op>(m.op);
  const bool top = m.depth == 0;
  const bool guest = op == w::Op::kPrepare || op == w::Op::kCall;
  const bool interruptible = guest || op == w::Op::kParseLibrary;
  if (top) {
    // The worker begins to serve the request: it has received it completely (§11.1).
    m_seq = m.call_seq;
    Interrupts::begin(m_seq, interruptible);
    if (interruptible && !m_options_in_force)
      protocol_failure("PREPARE, CALL or PARSE_LIBRARY without options in force");
  }
  Request q;
  q.op = m.op;
  q.depth = m.depth;
  q.scope = ++m_scopes;
  q.outer = m_request;
  m_request = &q;
  Reply reply;
  if (m.discarded) {
    too_large(m, q, &reply);
  } else {
    switch (op) {
      case w::Op::kPrepare:
        prepare(m, q, &reply);
        break;
      case w::Op::kCall:
        call(m, q, &reply);
        break;
      case w::Op::kParseLibrary:
        parse_library(m, &reply);
        break;
      case w::Op::kDrop:
        drop(m);
        break;
      case w::Op::kReset:
        reset(m);
        break;
      case w::Op::kOptions:
        options(m, &reply);
        break;
      default:
        protocol_failure("a message that is not a request");
    }
  }
  q.guest = false;  // no guest code of the request runs from here on
  if (interruptible) {
    // The status is fixed here (§11.1). INTERRUPTED only for an interrupt that was delivered and
    // that V8 acted on; one it has not acted on is cancelled, and the RESULT is what it is. A
    // nested request leaves the cancelled termination to sql(), which requests it again for the
    // outer code.
    const bool delivered = top ? Interrupts::end() : Interrupts::interrupted();
    if (std::exchange(m_exhausted, false)) {
      m_tainted = true;
      reply.fail(w::Status::kExhausted, kJs, "RangeError: out of memory (the V8 heap limit, mle.v8_heap_max)");
    } else if (delivered) {
      if (!termination_pending()) reply.fail(w::Status::kInterrupted, kJs, "interrupted");
    } else if (reply.terminated) {
      m_tainted = true;
      reply.fail(w::Status::kInternal, "", "V8 terminated the routine without an interrupt");
    }
    m_isolate->CancelTerminateExecution();
    if (top && guest && reply.status != w::Status::kOk) discard_microtasks();
  }
  post_result(q, reply);
  m_request = q.outer;
}

void Worker::too_large(const Message &m, Request &q, Reply *reply) {
  // §5.3: the transfer is complete and its chunks are gone; the worker itself is sound.
  if (static_cast<w::Op>(m.op) == w::Op::kPrepare) {
    q.routine_id = m.head;
    drop_routine(m.head);  // the id is not prepared (§8.3)
  }
  reply->fail(w::Status::kExhausted, "", "the request is larger than this MLE worker can hold");
}

void Worker::drop(const Message &m) {
  Reader r(m.body);
  const auto id = r.get<uint64_t>();
  r.finish();
  drop_routine(id);
}

void Worker::reset(const Message &m) {
  Reader(m.body).finish();  // an empty body
  m_routines.clear();
  m_answers.clear();
  // RESET ends the options (§9.8) and discards the console output that waits (§9.4). globalThis
  // and the loaded libraries stay (Appendix B.6).
  m_options = Options();
  m_options_in_force = false;
  m_out.clear();
  m_err.clear();
  m_cleared = false;
  m_isolate->LowMemoryNotification();  // a reset worker waits for reuse: give memory back
}

void Worker::options(const Message &m, Reply *reply) {
  Reader r(m.body);
  const std::string zone(r.str());
  const uint32_t n = r.count(w::kMaxSettings, 8);
  // The message carries the complete set: a setting that is absent has its default.
  Options o;
  std::string refused;
  for (uint32_t i = 0; i < n; i++) {
    const std::string_view name = r.str(), value = r.str();
    const auto choose = [&](std::initializer_list<std::string_view> values) {
      const auto at = std::find(values.begin(), values.end(), value);
      if (at != values.end()) return static_cast<int>(at - values.begin());
      if (refused.empty()) refused = "'" + std::string(value) + "' is not a value of " + std::string(name);
      return 0;
    };
    // Appendix B.2; a name of another language or worker kind is not this worker's.
    if (name == "js.integer_type")
      o.integer_type = static_cast<IntegerType>(choose({"unsafe_string", "unsafe_bigint", "string", "bigint"}));
    else if (name == "js.decimal_type")
      o.decimal_type = static_cast<DecimalType>(choose({"string", "number"}));
  }
  r.finish();
  // On a failure the options in force stay as they are.
  if (!o.set_zone(zone)) {
    reply->fail(w::Status::kGuestError, w::kLanguageTimeZone, "unknown time zone '" + zone + "'");
  } else if (!refused.empty()) {
    reply->fail(w::Status::kGuestError, w::kLanguageOption, refused);
  } else {
    m_options = o;
    m_options_in_force = true;
  }
}

void Worker::parse_library(const Message &m, Reply *reply) {
  Reader r(m.body);
  const std::string language(r.str()), name(r.str()), body(r.str());
  r.finish();
  if (language != kJs) protocol_failure("PARSE_LIBRARY in a language that READY does not list");
  v8::TryCatch tc(m_isolate);
  try {
    v8::ScriptOrigin origin(v8_string(m_isolate, name), 0, 0, false, -1, Local<Value>(), false, false, true);
    v8::ScriptCompiler::Source src(v8_string(m_isolate, body), origin);
    if (v8::ScriptCompiler::CompileModule(m_isolate, &src).IsEmpty()) {
      // Oracle's form: "SyntaxError: name:line:col message" (the first line).
      std::string message = compile_error(tc.Message(), name, 0);
      message = message.substr(0, message.find('\n'));
      reply->fail(w::Status::kGuestError, kJs, message);
    }
  } catch (const ConversionError &e) {
    reply->fail(w::Status::kGuestError, kJs, e.message);
  }
}

void Worker::prepare(const Message &m, Request &q, Reply *reply) {
  // The whole message is taken out of the arena before guest code, which may post, runs.
  Reader r(m.body);
  const auto id = r.get<uint64_t>();
  const std::string language(r.str()), name(r.str()), entry(r.str()), source(r.str());
  const uint32_t n = r.count(w::kMaxParams, 23);
  const std::shared_ptr<Routine> routine(new Routine, [this](Routine *gone) {
    m_scripts.erase(gone->script_id);  // its frames' source lines
    delete gone;
  });
  std::vector<std::string> names;
  std::vector<uint8_t> modes;
  for (uint32_t i = 0; i < n; i++) {
    names.emplace_back(r.str());
    modes.push_back(r.get<uint8_t>());
    if (modes.back() > uint8_t(w::ParamMode::kInOut)) protocol_failure("an unknown Param mode");
    routine->params.push_back(ParamType::read(r));
    if (modes.back() != uint8_t(w::ParamMode::kIn)) routine->outs.push_back(i);
  }
  routine->has_return = r.boolean();
  if (routine->has_return) routine->ret = ParamType::read(r);
  const uint32_t nimports = r.count(w::kMaxImports, 12);
  std::vector<std::string> usings;
  for (uint32_t i = 0; i < nimports; i++) {
    const std::string schema(r.str()), library(r.str()), alias(r.str());
    // USING: each library bound to its alias or name, loaded when the routine starts to run.
    usings.push_back("const " + (alias.empty() ? library : alias) + "=$c.lib(" + js_string(schema) + "," +
                     js_string(library) + ");");
  }
  routine->schema = r.str();
  (void)r.boolean();  // check: this worker has no findings beyond syntax
  q.library_epoch = r.get<uint64_t>();
  r.finish();
  if (language != kJs) protocol_failure("PREPARE in a language that READY does not list");
  q.routine_id = id;
  q.schema = routine->schema;
  q.guest = true;
  // The id is not prepared from here until this PREPARE answers OK (§8.3).
  drop_routine(id);
  stats(uint32_t(w::WorkerState::kCompiling));
  std::string compile_err;
  v8::TryCatch tc(m_isolate);
  if (Interrupts::interrupted()) m_isolate->TerminateExecution();
  try {
    compile(name.empty() ? "routine_" + std::to_string(id) : name, entry, source, *routine, names, modes, usings,
            &compile_err);
  } catch (const ConversionError &e) {
    compile_err = e.message;
  }
  if (!compile_err.empty()) {
    reply->fail(w::Status::kGuestError, kJs, compile_err);
  } else if (tc.HasCaught() || routine->fn.IsEmpty()) {
    failed(tc, reply);
  } else {
    // What loading the code queued runs within this request.
    m_isolate->PerformMicrotaskCheckpoint();
    if (tc.HasTerminated())
      failed(tc, reply);
    else
      m_routines[id] = routine;
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

void Worker::compile(const std::string &name, const std::string &entry, const std::string &source, Routine &r,
                     const std::vector<std::string> &names, const std::vector<uint8_t> &modes,
                     const std::vector<std::string> &usings, std::string *error) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  v8::TryCatch tc(m_isolate);
  tc.SetVerbose(false);
  if (entry == "<eval>") {
    // A strict-mode block; its completion value as text. Syntax errors of the code as written first.
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
    // Oracle's form: the body in an arrow function inside a wrapper whose parameters are the
    // routine's; globalThis fresh per call; all on the body's first line.
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

void Worker::call(const Message &m, Request &q, Reply *reply) {
  Reader r(m.body);
  q.routine_id = r.get<uint64_t>();
  q.library_epoch = r.get<uint64_t>();
  const uint32_t n = r.count(w::kMaxValues, 1);
  const auto it = m_routines.find(q.routine_id);
  if (it == m_routines.end() || it->second->params.size() != n)
    protocol_failure("a CALL of a routine that is not prepared with this number of parameters");
  // This activation keeps the code it starts with, whatever a nested PREPARE or DROP does (§8.3).
  q.routine = it->second;
  const Routine &routine = *q.routine;
  q.schema = routine.schema;
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  // The arguments are copies: nothing refers to the arena once guest code, which may post, runs.
  std::vector<Local<Value>> args(n);
  std::string unconvertible;
  for (uint32_t i = 0; i < n; i++) {
    try {
      args[i] = m_values->argument(r, routine.params[i]);
    } catch (const ConversionError &e) {
      if (unconvertible.empty()) unconvertible = e.message;
    }
  }
  r.finish();
  if (!unconvertible.empty()) {
    reply->fail(w::Status::kGuestError, w::kLanguageType, unconvertible);
    return;
  }
  q.guest = true;
  stats(uint32_t(w::WorkerState::kRunning));
  v8::TryCatch tc(m_isolate);
  // A nested request that begins after the interrupt of its top-level request does not run.
  if (Interrupts::interrupted()) m_isolate->TerminateExecution();
  Local<Value> ret;
  bool ok = routine.fn.Get(m_isolate)->Call(ctx, v8::Undefined(m_isolate), static_cast<int>(n), args.data()).ToLocal(&ret);
  Local<Value> thrown;
  Local<v8::Message> thrown_message;
  if (!ok) {
    if (tc.HasTerminated() || !tc.HasCaught()) {
      failed(tc, reply);
      return;
    }
    thrown = tc.Exception();
    thrown_message = tc.Message();
    tc.Reset();
  }
  // What the routine queued (promise jobs) runs within its request, also after it threw.
  m_isolate->PerformMicrotaskCheckpoint();
  if (tc.HasTerminated()) {
    failed(tc, reply);
    return;
  }
  if (ok && routine.async && ret->IsPromise()) {
    const Local<v8::Promise> p = ret.As<v8::Promise>();
    if (p->State() == v8::Promise::kFulfilled) {
      ret = p->Result();
    } else if (p->State() == v8::Promise::kRejected) {
      ok = false;
      thrown = p->Result();
    } else {
      reply->fail(w::Status::kGuestError, w::kLanguageType,
                  q.depth > 0 ? "an async JavaScript routine (one that uses await) cannot run while another routine "
                                "of this worker runs (a nested call); its promise did not settle"
                              : "the routine's promise did not settle");
      return;
    }
  }
  if (!ok) {
    reply->status = w::Status::kGuestError;
    reply->error = error_of(thrown, thrown_message);
    if (tc.HasTerminated()) failed(tc, reply);
    return;
  }
  // The values: has_return [, value], out_params. Converting them may run guest code (toString,
  // toJSON), so it is done here, while the request can still be interrupted.
  Local<v8::Array> outs;
  if (routine.eval) {
    Local<Value> shown;
    if (!m_show.Get(m_isolate)->Call(ctx, v8::Undefined(m_isolate), 1, &ret).ToLocal(&shown)) shown = v8::Null(m_isolate);
    ret = shown;
  } else if (!routine.outs.empty()) {
    if (!ret->IsArray()) {
      m_tainted = true;
      reply->fail(w::Status::kInternal, "", "the routine's wrapper did not return its OUT values");
      return;
    }
    outs = ret.As<v8::Array>();
    ret = outs->Get(ctx, 0).ToLocalChecked();
  }
  Writer wr(reply->values, w::kMaxWorkerMessage);
  const auto need = [](bool fits) {
    if (!fits) throw TooLarge{};
  };
  try {
    const Converting converting(m_converting);
    if (routine.has_return || routine.eval) {
      need(wr.put<uint8_t>(1));
      ParamType text;
      text.tag = w::Tag::kString;
      m_values->write_as(wr, ret, routine.eval ? text : routine.ret);
    } else {
      need(wr.put<uint8_t>(0));
    }
    // Every OUT and INOUT parameter, in ascending index (§9.4).
    need(wr.put(static_cast<uint32_t>(routine.outs.size())));
    for (size_t k = 0; k < routine.outs.size(); k++) {
      const uint32_t i = routine.outs[k];
      need(wr.put(i));
      m_values->write_as(wr, outs->Get(ctx, static_cast<uint32_t>(k + 1)).ToLocalChecked(), routine.params[i]);
    }
    reply->values.resize(wr.size());
  } catch (const ConversionError &c) {
    reply->fail(w::Status::kGuestError, w::kLanguageType, c.message);
  } catch (const TooLarge &) {
    reply->fail(w::Status::kGuestError, w::kLanguageType, "the routine's result is larger than MLE allows (64 MiB)");
  }
  if (tc.HasTerminated()) failed(tc, reply);
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

void Worker::hand_back(const v8::FunctionCallbackInfo<Value> &info) {
  if (info.Length() < 5 || !info[0]->IsObject() || !info[1]->IsBigInt()) return;
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  // The ERROR's own number, SQLSTATE and message, with the top-level request the id belongs to.
  Local<Value> held[] = {info[1], v8::BigInt::NewFromUnsigned(m_isolate, m_seq), info[2], info[3], info[4]};
  (void)info[0].As<v8::Object>()->SetPrivate(ctx, m_handed.Get(m_isolate), v8::Array::New(m_isolate, held, std::size(held)));
}

ErrorInfo Worker::error_of(Local<Value> exception, Local<v8::Message> message) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  ErrorInfo e;
  e.language = kJs;
  e.message = m_values->str(exception);
  if (exception->IsObject()) {
    const Local<v8::Object> o = exception.As<v8::Object>();
    const auto valid_state = [](const std::string &s) {
      return s.size() == 5 &&
             std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); });
    };
    Local<Value> held, mark;
    if (o->GetPrivate(ctx, m_handed.Get(m_isolate)).ToLocal(&held) && held->IsArray()) {
      // A SqlError made from an SQL_RESULT ERROR (§10.8): that ERROR's number, SQLSTATE and
      // message, whatever guest code did to the object, and its id while the host can know it.
      const Local<v8::Array> h = held.As<v8::Array>();
      const auto at = [&](uint32_t i) { return h->Get(ctx, i).ToLocalChecked(); };
      const double code = at(2)->IsNumber() ? at(2).As<v8::Number>()->Value() : 0;
      e.sql_errno = code >= 1 && code <= w::kMaxSqlErrno ? static_cast<uint32_t>(code) : 0;
      e.sql_state = utf8(m_isolate, at(3));
      if (!valid_state(e.sql_state)) e.sql_state.clear();
      e.message = utf8(m_isolate, at(4));
      if (at(1).As<v8::BigInt>()->Uint64Value() == m_seq) e.sql_error_id = at(0).As<v8::BigInt>()->Uint64Value();
    } else if (o->Get(ctx, v8_string(m_isolate, "$mle$sql")).ToLocal(&mark) && mark->IsTrue()) {
      // A SqlError the routine raises itself (the SQL API's class: code and sqlState).
      Local<Value> code, state, text;
      if (o->Get(ctx, v8_string(m_isolate, "message")).ToLocal(&text)) e.message = m_values->str(text);
      if (o->Get(ctx, v8_string(m_isolate, "code")).ToLocal(&code) && code->IsNumber()) {
        // SIGNAL's MYSQL_ERRNO range; anything else (NaN from new SqlError(s, m, 'x')): 1644, SIGNAL's own.
        const double c = code.As<v8::Number>()->Value();
        e.sql_errno = c >= 1 && c <= w::kMaxSqlErrno && c == std::floor(c) ? static_cast<uint32_t>(c) : 1644;
      }
      if (o->Get(ctx, v8_string(m_isolate, "sqlState")).ToLocal(&state) && state->IsString()) {
        e.sql_state = utf8(m_isolate, state);
        if (!valid_state(e.sql_state)) {
          std::string shown = e.sql_state;
          cut(shown, 64);
          e.message += " (SqlError with the invalid SQLSTATE '" + shown + "')";
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

bool Worker::failed(v8::TryCatch &tc, Reply *reply) {
  if (tc.HasTerminated()) {
    // By an interrupt or the heap limit: handle() knows which and sets the status.
    reply->fail(w::Status::kInternal, "", "V8 terminated the routine");
    reply->terminated = true;
    return false;
  }
  if (!tc.HasCaught()) {
    m_tainted = true;
    reply->fail(w::Status::kInternal, "", "the routine failed without an exception");
    return true;
  }
  reply->status = w::Status::kGuestError;
  reply->values.clear();
  reply->error = error_of(tc.Exception(), tc.Message());
  if (tc.HasTerminated()) return failed(tc, reply);  // while the error was described
  return true;
}

// ------------------------------------------------------------ results

void Worker::write(int stream, std::string_view text) {
  std::string &out = stream == 2 ? m_err : m_out;
  out.append(text);
  // The end of the stream is kept; trimmed in steps, not at every write.
  if (out.size() > 2 * kConsoleLimit) out.erase(0, out.size() - kConsoleLimit);
}

void Worker::clear_console() {
  m_out.clear();
  m_err.clear();
  m_cleared = true;
}

void Worker::post_result(const Request &q, Reply &reply) {
  const auto op = static_cast<w::Op>(q.op);
  // Only the RESULTs of PREPARE and CALL carry console output: all that no RESULT has carried
  // yet, at whatever depth it was printed (§9.4). Other output waits.
  const bool console = op == w::Op::kPrepare || op == w::Op::kCall;
  ErrorInfo &e = reply.error;
  const bool has_error = reply.status != w::Status::kOk;
  const auto error_size = [&e]() {
    size_t n = 4 + e.language.size() + 4 + e.message.size() + 4 + 4 + 4 + e.sql_state.size() + 8;
    for (const ErrorInfo::Frame &f : e.frames) n += 4 + f.function.size() + 4 + f.file.size() + 12 + 4 + f.text.size();
    return n;
  };
  if (has_error) {
    // The caps of §7.4. An Error within them (16 KiB, and no frames outside PREPARE and CALL)
    // also fits the 64 KiB of a RESULT of DROP, RESET and OPTIONS.
    cut(e.message, w::kMaxErrorMessage);
    if (e.frames.size() > w::kMaxStackFrames) e.frames.resize(w::kMaxStackFrames);
    for (ErrorInfo::Frame &f : e.frames) {
      cut(f.function, w::kMaxFrameText);
      cut(f.file, w::kMaxFrameText);
      cut(f.text, w::kMaxFrameText);
    }
  }
  // has_return, out_params count; stdout and stderr lengths, console_cleared; findings count; has_error.
  constexpr size_t kFixed = 4 + 4 + 1 + 4 + 1;
  if (reply.values.empty()) reply.values.assign(5, std::byte{0});  // no return value, no OUT values
  if (reply.values.size() + kFixed > w::kMaxWorkerMessage) {
    // The values alone leave no room (§9.4).
    reply.fail(w::Status::kGuestError, w::kLanguageType, "the routine's result is larger than MLE allows (64 MiB)");
    reply.values.assign(5, std::byte{0});
  }
  const size_t rest = reply.values.size() + kFixed + (reply.status != w::Status::kOk ? error_size() : 0);
  // Console output beyond the message's limit is cut, the end of each stream kept, and dropped.
  if (m_out.size() > kConsoleLimit) m_out.erase(0, m_out.size() - kConsoleLimit);
  if (m_err.size() > kConsoleLimit) m_err.erase(0, m_err.size() - kConsoleLimit);
  std::string_view out, err;
  if (console) {
    const size_t room = w::kMaxWorkerMessage - rest;
    const size_t err_room = std::min(m_err.size(), std::max(room / 2, room - std::min(room, m_out.size())));
    const size_t out_room = std::min(m_out.size(), room - err_room);
    out = std::string_view(m_out).substr(m_out.size() - out_room);
    err = std::string_view(m_err).substr(m_err.size() - err_room);
  }
  Writer wr(m_result_buf, w::kMaxWorkerMessage);
  bool fits = wr.bytes(reply.values) && wr.str(out) && wr.str(err) &&
              wr.put<uint8_t>(console && m_cleared ? 1 : 0) && wr.put<uint32_t>(0);  // no findings
  if (reply.status == w::Status::kOk) {
    fits = fits && wr.put<uint8_t>(0);
  } else {
    fits = fits && wr.put<uint8_t>(1) && wr.str(e.language) && wr.str(e.message) &&
           wr.put(static_cast<uint32_t>(e.frames.size()));
    for (const ErrorInfo::Frame &f : e.frames)
      fits = fits && wr.str(f.function) && wr.str(f.file) && wr.put(f.line) && wr.put(f.column) &&
             wr.put(f.end_column) && wr.str(f.text);
    fits = fits && wr.put(e.sql_errno) && wr.str(e.sql_state) && wr.put(e.sql_error_id);
  }
  if (!fits) protocol_failure("worker error: a RESULT over the size limit");
  // tainted is set before the RESULT (§11.5).
  stats(q.depth == 0 ? uint32_t(w::WorkerState::kIdle) : uint32_t(w::WorkerState::kRunning));
  Mailbox::send(uint32_t(w::Op::kResult), uint32_t(reply.status), q.depth, m_seq, wr.written());
  if (console) {
    m_out.clear();
    m_err.clear();
    m_cleared = false;
  }
  if (m_result_buf.capacity() > (4U << 20)) std::vector<std::byte>().swap(m_result_buf);
}

// ------------------------------------------------------------ SQL

Local<Value> Worker::state() {
  const Request *q = m_request;
  Local<Value> items[] = {
      v8::Number::New(m_isolate, q == nullptr ? 0 : q->depth),
      v8::Number::New(m_isolate, q == nullptr ? 0 : static_cast<double>(q->scope)),
      q == nullptr || q->schema.empty() ? Local<Value>(v8::Null(m_isolate))
                                        : Local<Value>(v8_string(m_isolate, q->schema)),
      v8::Number::New(m_isolate, static_cast<double>(m_options.integer_type)),
      v8::Number::New(m_isolate, static_cast<double>(m_options.decimal_type)),
  };
  return v8::Array::New(m_isolate, items, std::size(items));
}

namespace {

/** A list of Warning as [[level, code, message], ...]. */
Local<Value> warnings_of(v8::Isolate *iso, Reader &r) {
  const uint32_t n = r.count(w::kMaxWarnings, 12);
  const Local<v8::Array> a = v8::Array::New(iso, static_cast<int>(n));
  const Local<v8::Context> ctx = iso->GetCurrentContext();
  for (uint32_t i = 0; i < n; i++) {
    const auto level = r.get<uint32_t>(), code = r.get<uint32_t>();
    if (level > uint32_t(w::WarningLevel::kError)) protocol_failure("an unknown Warning level");
    Local<Value> warning[] = {v8::Number::New(iso, level), v8::Number::New(iso, code), v8_string(iso, r.str())};
    (void)a->Set(ctx, i, v8::Array::New(iso, warning, 3));
  }
  return a;
}

Local<Value> number(v8::Isolate *iso, uint64_t v) {
  return v <= (uint64_t{1} << 53) ? Local<Value>(v8::Number::New(iso, static_cast<double>(v)))
                                   : Local<Value>(v8::BigInt::NewFromUnsigned(iso, v));
}

}  // namespace

Message Worker::sql_reply(std::vector<std::byte> &assembled) {
  while (true) {
    const Message m = Mailbox::receive([&](size_t total) { return allocate(assembled, total); });
    if (static_cast<w::Op>(m.op) == w::Op::kSqlResult) return m;
    // A nested PREPARE, CALL or DROP (the mailbox has checked that it is one), served on this
    // thread from inside the wait (§10.6).
    handle(m);
  }
}

void Worker::sql_close(Request &q, uint64_t id) {
  std::array<std::byte, w::kSqlCloseSize> close{};
  Writer wr(close);
  (void)(wr.put(uint8_t(w::SqlAction::kClose)) && wr.put(id));  // no text, no params, flags and the rest 0
  Mailbox::send(uint32_t(w::Op::kSql), 0, q.depth, m_seq, close);
  std::vector<std::byte> assembled;
  const Message m = sql_reply(assembled);
  if (m.discarded) protocol_failure("the SQL_RESULT of a CLOSE in chunks");
  (void)sql_result(q, w::SqlAction::kClose, id, 0, 0, m, m_options);
}

v8::MaybeLocal<Value> Worker::sql_result(Request &q, w::SqlAction action, uint64_t id, uint8_t flags,
                                         uint32_t rows_sent, const Message &m, const Options &rules) {
  using State = Statement::State;
  v8::Isolate *iso = m_isolate;
  const Local<v8::Context> ctx = m_context.Get(iso);
  Reader r(m.body);
  const auto kind = r.get<uint8_t>();
  if (kind > w::kMaxSqlResultKind) protocol_failure("an unknown SQL_RESULT kind");
  const auto found = q.statements.find(id);
  Statement *st = found == q.statements.end() ? nullptr : &found->second;  // nullptr: CLOSED
  // §10.1: is the action allowed in the id's state, and may this kind answer it?
  bool allowed = true;
  if (action == w::SqlAction::kExecutePrepared || action == w::SqlAction::kExecuteMany)
    allowed = st != nullptr && st->prepared;
  else if (action == w::SqlAction::kFetch)
    allowed = st != nullptr && st->state == State::kRows;
  const bool to_client = st != nullptr ? st->to_client : (flags & w::kPassResultToClient) != 0;
  const bool nothing_next = action == w::SqlAction::kNextResult && (st == nullptr || st->state == State::kIdle);
  bool legal = false;
  switch (static_cast<w::SqlResultKind>(kind)) {
    case w::SqlResultKind::kError:
      legal = action != w::SqlAction::kClose;
      break;
    case w::SqlResultKind::kOk:
      legal = allowed && action != w::SqlAction::kFetch && !nothing_next;
      break;
    case w::SqlResultKind::kRows:
      legal = allowed && !to_client && !nothing_next && action != w::SqlAction::kPrepare &&
              action != w::SqlAction::kExecuteMany && action != w::SqlAction::kClose;
      break;
    case w::SqlResultKind::kEnd:
      legal = action == w::SqlAction::kNextResult;
      break;
  }
  if (!legal) protocol_failure("an SQL_RESULT of a kind that cannot answer the action in the statement's state");
  // The id's home: IDLE for a prepared statement, CLOSED for a direct one.
  const auto home = [&]() {
    if (st == nullptr) return;
    if (st->prepared) {
      st->state = State::kIdle;
      st->columns.clear();
    } else {
      q.statements.erase(id);
    }
    st = nullptr;
  };
  // The id has a result to read: a direct statement gets its entry now.
  const auto open = [&]() -> Statement & {
    if (st == nullptr) {
      st = &q.statements[id];
      st->to_client = to_client;
    }
    return *st;
  };
  const Local<Value> scope = v8::Number::New(iso, static_cast<double>(q.scope));
  const Local<Value> handle = v8::Number::New(iso, static_cast<double>(id));
  switch (static_cast<w::SqlResultKind>(kind)) {
    case w::SqlResultKind::kError: {
      const auto code = r.get<uint32_t>();
      const std::string_view state = r.str(), message = r.str();
      const auto row = r.get<uint32_t>();
      const auto error_id = r.get<uint64_t>();
      r.finish();
      if (code < 1 || code > w::kMaxSqlErrno || state.size() != 5 || error_id == 0 ||
          (action == w::SqlAction::kExecuteMany ? row > rows_sent : row != w::kNoRow))
        protocol_failure("an SQL_RESULT ERROR outside its domain");
      Local<Value> e[] = {v8::Number::New(iso, 0), v8::Number::New(iso, code), v8_string(iso, state),
                          v8_string(iso, message),
                          v8::Number::New(iso, row == w::kNoRow ? -1.0 : static_cast<double>(row)),
                          v8::BigInt::NewFromUnsigned(iso, error_id)};
      // Rule 2: an action that is not allowed changes nothing. Rule 3: the id goes home.
      if (allowed) home();
      return v8::Array::New(iso, e, std::size(e));
    }
    case w::SqlResultKind::kOk: {
      const auto affected = r.get<uint64_t>(), insert_id = r.get<uint64_t>();
      const size_t before = r.pos();
      const Local<Value> warns = warnings_of(iso, r);
      const bool no_warnings = r.pos() == before + 4;
      const bool more_results = r.boolean();
      const auto pcount = r.get<uint32_t>();
      const auto exec_us = r.get<uint64_t>();
      r.finish();
      const bool bare = action == w::SqlAction::kPrepare || action == w::SqlAction::kClose;
      if ((action == w::SqlAction::kPrepare ? pcount > w::kMaxParams : pcount != 0) ||
          (bare && (affected != 0 || insert_id != 0 || !no_warnings || more_results)) ||
          ((to_client || action == w::SqlAction::kExecuteMany) && more_results))
        protocol_failure("an SQL_RESULT OK outside its domain");
      if (action == w::SqlAction::kClose) {
        q.statements.erase(id);
      } else if (action == w::SqlAction::kPrepare) {
        open().prepared = true;
      } else if (more_results) {
        open().state = State::kNext;
        st->columns.clear();
      } else {
        home();
      }
      Local<Value> o[] = {v8::Number::New(iso, 1), number(iso, affected), number(iso, insert_id), warns,
                          v8::Boolean::New(iso, more_results), v8::Number::New(iso, pcount),
                          v8::Number::New(iso, static_cast<double>(exec_us)), scope, handle};
      return v8::Array::New(iso, o, std::size(o));
    }
    case w::SqlResultKind::kRows: {
      // The first batch of a result set carries its columns; a batch that answers FETCH has none
      // and the set's total_rows again (§10.3).
      const bool first = action != w::SqlAction::kFetch;
      const uint32_t ncols = r.count(w::kMaxColumns, 50);
      if (first ? ncols == 0 : ncols != 0) protocol_failure("a batch of rows with its columns out of place");
      std::vector<ParamType> types;
      Local<Value> labels = v8::Null(iso), columns = v8::Null(iso);
      if (first) {
        const Local<v8::Array> label_list = v8::Array::New(iso, static_cast<int>(ncols));
        const Local<v8::Array> column_list = v8::Array::New(iso, static_cast<int>(ncols));
        for (uint32_t i = 0; i < ncols; i++) {
          const std::string_view name = r.str(), table = r.str();
          ParamType t = ParamType::read(r);
          const std::string_view org_name = r.str(), schema = r.str(), org_table = r.str(), charset = r.str(),
                                 collation = r.str();
          const auto column_flags = r.get<uint32_t>();
          const Local<v8::String> label = v8_string(iso, name);
          (void)label_list->Set(ctx, i, label);
          Local<Value> c[] = {label,                          v8_string(iso, table),
                              v8::Number::New(iso, static_cast<double>(t.tag)), v8::Boolean::New(iso, t.is_unsigned),
                              v8::Number::New(iso, t.length), v8::Number::New(iso, t.precision),
                              v8::Number::New(iso, t.scale),  v8_string(iso, t.type_name),
                              v8_string(iso, org_name),       v8_string(iso, schema),
                              v8_string(iso, org_table),      v8_string(iso, charset),
                              v8_string(iso, collation),      v8::Number::New(iso, column_flags)};
          (void)column_list->Set(ctx, i, v8::Array::New(iso, c, std::size(c)));
          types.push_back(std::move(t));
        }
        labels = label_list;
        columns = column_list;
      }
      const std::vector<ParamType> &set = first ? types : st->columns;
      const auto total = r.get<uint64_t>();
      if (!first && total != st->total_rows) protocol_failure("a later batch with another total_rows");
      const auto nrows = r.get<uint32_t>();
      const uint64_t count = uint64_t{nrows} * set.size();
      if (count > r.left()) protocol_failure("more row values than the message holds");
      // The rows' values in one flat array, row by row: sql.js makes the rows, in JavaScript,
      // where V8 compiles their shape once. A value JavaScript cannot hold fails the statement in
      // guest code after the whole reply is read, so that the id's state is known.
      std::vector<Local<Value>> values(static_cast<size_t>(count));
      std::string unconvertible;
      for (size_t k = 0; k < values.size(); k++) {
        try {
          values[k] = m_values->column(r, set[k % set.size()], &rules);
        } catch (const ConversionError &c) {
          if (unconvertible.empty()) unconvertible = c.message;
          values[k] = v8::Undefined(iso);
        }
      }
      const bool more = r.boolean(), more_results = r.boolean();
      const Local<Value> warns = warnings_of(iso, r);
      const auto exec_us = r.get<uint64_t>();
      r.finish();
      if (more && (nrows == 0 || more_results)) protocol_failure("a batch with more = 1 that is empty or has more_results");
      Local<Value> o[] = {v8::Number::New(iso, 2),
                          labels,
                          columns,
                          total == w::kUnknownTotalRows ? Local<Value>(v8::Null(iso)) : number(iso, total),
                          v8::Array::New(iso, values.data(), values.size()),
                          v8::Boolean::New(iso, more),
                          v8::Boolean::New(iso, more_results),
                          warns,
                          v8::Number::New(iso, static_cast<double>(exec_us)),
                          scope,
                          handle};
      if (more || more_results) {
        Statement &s = open();
        s.state = more ? State::kRows : State::kNext;
        if (first) {
          s.columns = std::move(types);
          s.total_rows = total;
        }
        if (!more) s.columns.clear();
      } else {
        home();
      }
      if (!unconvertible.empty()) {
        iso->ThrowException(v8::Exception::TypeError(v8_string(iso, unconvertible)));
        return {};
      }
      return v8::Array::New(iso, o, std::size(o));
    }
    case w::SqlResultKind::kEnd: {
      r.finish();
      home();
      Local<Value> o[] = {v8::Number::New(iso, 3)};
      return v8::Array::New(iso, o, 1);
    }
  }
  protocol_failure("an unknown SQL_RESULT kind");
}

v8::MaybeLocal<Value> Worker::sql(const v8::FunctionCallbackInfo<Value> &info) {
  v8::Isolate *iso = m_isolate;
  const Local<v8::Context> ctx = m_context.Get(iso);
  const auto fail = [&](Local<Value> exception) {
    iso->ThrowException(exception);
    return v8::MaybeLocal<Value>();
  };
  const auto type_error = [&](const std::string &text) { return fail(v8::Exception::TypeError(v8_string(iso, text))); };
  // SQL comes only from the guest code of the request being served (§10.1).
  if (const char *why = host_unavailable(); why != nullptr) return fail(v8::Exception::Error(v8_string(iso, why)));
  Request &q = *m_request;
  // Everything of the message is decided here: what guest code may have changed in sql.js or in
  // the built-ins it uses cannot make the worker send an illegal action, flag or field (§10.6).
  if (info.Length() < 9) return type_error("sql: 9 arguments");
  const auto integer = [&](int i, double max) -> std::optional<uint64_t> {
    if (!info[i]->IsNumber()) return std::nullopt;
    const double v = info[i].As<v8::Number>()->Value();
    if (!(v >= 0 && v <= max && v == std::floor(v))) return std::nullopt;
    return static_cast<uint64_t>(v);
  };
  const std::optional<uint64_t> action_code = integer(0, w::kMaxSqlAction);
  if (!action_code) return type_error("sql: unknown action");
  const auto action = static_cast<w::SqlAction>(*action_code);
  const bool opens = action == w::SqlAction::kExecute || action == w::SqlAction::kPrepare;
  const bool has_params = action == w::SqlAction::kExecute || action == w::SqlAction::kExecutePrepared ||
                          action == w::SqlAction::kExecuteMany;
  uint64_t id = 0;
  if (!opens) {
    // A statement belongs to the request that opened it: the handle of another request (an
    // earlier one, or the caller's) fails here, and nothing is sent.
    const std::optional<uint64_t> scope = integer(1, 9e15), handle = integer(2, static_cast<double>(q.statement_ids));
    if (!scope || *scope != q.scope)
      return fail(v8::Exception::Error(
          v8_string(iso, "the SQL statement or result belongs to another routine call (one that ended, or the caller)")));
    if (!handle || *handle == 0) return type_error("sql: unknown statement");
    id = *handle;
  }
  std::string text;
  uint8_t flags = 0;
  if (opens) {
    // SQL text is code: it is never repaired (§7.1).
    if (!info[3]->IsString()) return type_error("sql: the statement must be a string");
    std::optional<std::string> exact = utf8_exact(iso, info[3].As<v8::String>());
    if (!exact) return type_error("the SQL statement is not well-formed Unicode (an unpaired surrogate)");
    text = std::move(*exact);
    const std::optional<uint64_t> f = integer(5, w::kKnownSqlFlags);
    if (!f) return type_error("sql: unknown flags");
    flags = static_cast<uint8_t>(*f);
  }
  uint32_t width = 0;
  if (action == w::SqlAction::kExecuteMany) {
    const std::optional<uint64_t> v = integer(6, w::kMaxValues);
    if (!v) return type_error("sql: bad row width");
    width = static_cast<uint32_t>(*v);
  }
  Local<v8::Array> params;
  uint32_t nparams = 0;
  if (has_params && info[4]->IsArray()) {
    params = info[4].As<v8::Array>();
    nparams = params->Length();
  }
  if (nparams > w::kMaxValues)
    return type_error("Incorrect arguments: " + std::to_string(nparams) + " values in one statement (at most " +
                      std::to_string(w::kMaxValues) + ")");
  // The statement's integerType and decimalType; anything else: the session's.
  Options rules = m_options;
  if (const std::optional<uint64_t> v = integer(7, kMaxIntegerType); v) rules.integer_type = static_cast<IntegerType>(*v);
  if (const std::optional<uint64_t> v = integer(8, kMaxDecimalType); v) rules.decimal_type = static_cast<DecimalType>(*v);
  if (opens) id = ++q.statement_ids;

  // The message, composed where guest code (a parameter's toJSON) cannot post.
  Writer wr(m_sql_buf, w::kMaxWorkerMessage);
  try {
    const Converting converting(m_converting);
    if (!(wr.put(static_cast<uint8_t>(action)) && wr.put(id) && wr.str(text) && wr.put(nparams))) throw TooLarge{};
    for (uint32_t i = 0; i < nparams; i++) {
      Local<Value> p;
      if (!params->Get(ctx, i).ToLocal(&p)) return {};
      m_values->write_param(wr, p);
    }
    if (!(wr.put(flags) && wr.put(uint32_t{0}) && wr.put(width))) throw TooLarge{};
  } catch (const TooLarge &) {
    // A statement that does not fit a message gets an error from the worker, not a message
    // (§10.6): MySQL's error for a packet over max_allowed_packet, which sql.js makes a SqlError.
    std::vector<std::byte>().swap(m_sql_buf);
    Local<Value> e[] = {v8::Number::New(iso, 0),
                        v8::Number::New(iso, 1153),
                        v8_string(iso, "HY000"),
                        v8_string(iso, "the SQL statement with its parameters is larger than MLE allows (" +
                                           std::to_string(w::kMaxWorkerMessage >> 20) + " MiB)"),
                        v8::Number::New(iso, -1),
                        v8::BigInt::New(iso, 0)};
    return v8::Array::New(iso, e, std::size(e));
  } catch (const ConversionError &c) {
    return type_error(c.message);
  }
  if (iso->IsExecutionTerminating()) return {};
  Mailbox::send(uint32_t(w::Op::kSql), 0, q.depth, m_seq, wr.written());
  if (m_sql_buf.capacity() > (4U << 20)) std::vector<std::byte>().swap(m_sql_buf);
  // An interrupt that stopped a nested routine stops this one too, once back in JavaScript: it
  // could catch the error of its statement and run on (§11.1).
  struct Rearm {
    v8::Isolate *iso;
    ~Rearm() {
      if (Interrupts::interrupted()) iso->TerminateExecution();
    }
  } rearm{iso};
  std::vector<std::byte> assembled;
  const Message m = sql_reply(assembled);
  if (m.discarded) {
    // §5.3: the worker cannot know the id's state: it closes the id and fails the statement.
    sql_close(q, id);
    return fail(v8::Exception::RangeError(
        v8_string(iso, "the result of the SQL statement is larger than this MLE worker can hold")));
  }
  try {
    const uint32_t rows_sent = action == w::SqlAction::kExecuteMany && width != 0 ? nparams / width : 0;
    return sql_result(q, action, id, flags, rows_sent, m, rules);
  } catch (const ConversionError &c) {
    return type_error(c.message);
  }
}

}  // namespace mle::v8w
