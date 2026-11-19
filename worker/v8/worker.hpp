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
  @file worker/v8/worker.hpp

  The V8 worker's core (exp design note 63; Dispatcher.java's role for LANGUAGE JS): one isolate
  and context on the main thread; the request loop (PREPARE, CALL, DROP, RESET, OPTIONS,
  PARSE_LIBRARY); routines in Oracle's form (JsOracle.java); results, errors and the console;
  KILL through Isolate::TerminateExecution; the heap limit as an error that retires the worker.
*/

#ifndef MLE_V8_WORKER_HPP
#define MLE_V8_WORKER_HPP

#include <libplatform/libplatform.h>
#include <v8.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/mailbox.hpp"
#include "values.hpp"

namespace mle::v8w {

struct Config {
  std::string cls = "v8";
  int spin = 2000;
  uint64_t heap_max = 512ULL << 20;
  /** The DBA's npm packages (<package_dir>/js); empty: none. */
  std::string packages;
};

/** A guest error for the RESULT (protocol.h: Error). */
struct ErrorInfo {
  std::string language;
  std::string message;
  struct Frame {
    std::string function, file;
    uint32_t line = 0, column = 0, end_column = 0;
    std::string text;
  };
  std::vector<Frame> frames;
  uint32_t sql_errno = 0;
  std::string sql_state;
};

class Worker {
 public:
  explicit Worker(const Config &config);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  /** READY, then requests until the host is gone (the control thread ends the process). */
  [[noreturn]] void run();

  /** The control thread's KILL (Mailbox hooks): any thread. */
  void terminate() { m_isolate->TerminateExecution(); }
  /** The request ended before the KILL reached it: in the serving thread. */
  void cancel_terminate() { m_isolate->CancelTerminateExecution(); }

  // Natives (api.js's N).
  void write(int stream, std::string_view text);
  void clear_console();
  /** N.sql(action, id, text, params, flags, width, integerType, decimalType, protoFor): one SQL
      request (protocol.h kSql); serves the host's nested requests while it waits; the reply as an
      array (see worker.cc). Throws (a V8 exception, returns empty) on a value that cannot be bound
      or a message larger than the host takes. */
  v8::MaybeLocal<v8::Value> sql(const v8::FunctionCallbackInfo<v8::Value> &info);
  /** N.state(): [depth, scope, routine schema, integer type, decimal type, arena size]. */
  v8::Local<v8::Value> state();

  static Worker *current() { return s_current; }

 private:
  struct Routine {
    v8::Global<v8::Function> fn;
    std::vector<ParamType> params;
    std::vector<uint32_t> outs;  // parameter indexes of OUT and INOUT
    bool has_return = false;
    ParamType ret;
    bool async = false;
    bool eval = false;  // mle_eval: the value as text
    int script_id = 0;
  };
  /** A routine's script: its name and source lines (line 1 without the wrapper), for frames. */
  struct Script {
    std::string name;
    int prefix = 0;
    std::vector<std::string> lines;
  };

  void ready(uint32_t status, const std::string &info);
  void handle(const common::Message &m);
  void prepare(const common::Message &m);
  void call(const common::Message &m);
  void options(const common::Message &m);
  void parse_library(const common::Message &m);

  /** Compiles a routine; on a compile error *error says it (and the function is empty). */
  void compile(uint64_t id, const std::string &name, const std::string &entry, const std::string &source,
               Routine &r, const std::vector<std::string> &param_names, const std::vector<uint8_t> &modes,
               std::string *error);
  /** A script of ours (routine, eval) compiled and run; registers it for frames. */
  v8::MaybeLocal<v8::Value> run_script(const std::string &text, const std::string &name, int prefix,
                                       int *script_id);

  /** The error of a caught exception (or a termination). */
  void failed(v8::TryCatch &tc, bool interrupted);
  ErrorInfo error_of(v8::Local<v8::Value> exception, v8::Local<v8::Message> message);
  std::string compile_error(v8::Local<v8::Message> message, const std::string &name, int prefix);

  /** Posts a RESULT (status OK: the return value and OUT values given). */
  void result(uint32_t status, const ErrorInfo *error, const Routine *r = nullptr,
              v8::Local<v8::Value> ret = {}, v8::Local<v8::Array> outs = {});
  void write_result(Writer &w, const ErrorInfo *error, const Routine *r, v8::Local<v8::Value> ret,
                    v8::Local<v8::Array> outs);
  void stats(uint32_t state);

  static inline Worker *s_current = nullptr;
  static size_t near_heap_limit(void *data, size_t current, size_t initial);

  Config m_config;
  std::unique_ptr<v8::Platform> m_platform;
  std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
  v8::Isolate *m_isolate = nullptr;
  v8::Global<v8::Context> m_context;
  v8::Global<v8::Object> m_api;      // api.js's helpers
  v8::Global<v8::Function> m_show;   // mle_eval's text of a value
  v8::Global<v8::Function> m_call_ended;  // sql.js: the end of a routine call
  Options m_options;
  std::unique_ptr<Values> m_values;
  std::unordered_map<uint64_t, Routine> m_routines;
  std::unordered_map<int, Script> m_scripts;
  std::string m_out, m_err;
  bool m_cleared = false;
  bool m_tainted = false;
  bool m_exhausted = false;
  uint32_t m_depth = 0;
  uint64_t m_seq = 0;
  /** Each request handled gets a scope (options set in a call apply to it only, C15). */
  uint64_t m_scope = 0, m_scopes = 0;
  /** The routine running (or loading) and each routine's schema (PREPARE). */
  uint64_t m_routine = 0;
  std::unordered_map<uint64_t, std::string> m_schemas;
  std::vector<std::byte> m_sql_buf;
  uint32_t m_calls = 0;
  std::vector<std::byte> m_big;  // a RESULT larger than the arena
};

}  // namespace mle::v8w

#endif  // MLE_V8_WORKER_HPP
