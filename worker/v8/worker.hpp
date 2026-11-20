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

  The V8 worker's core: one isolate and context on the main thread; the requests of the MLE worker
  protocol (docs/protocol.md: PREPARE, CALL, DROP, RESET, OPTIONS, PARSE_LIBRARY) and their RESULTs;
  routines in Oracle's form; errors and the console; SQL (§10) with the statement states; database
  libraries (§9.10, modules.cc); interruption through Isolate::TerminateExecution; the heap limit
  as EXHAUSTED, which retires the worker.
*/

#ifndef MLE_V8_WORKER_HPP
#define MLE_V8_WORKER_HPP

#include <libplatform/libplatform.h>
#include <v8.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/mailbox.hpp"
#include "values.hpp"

namespace mle::v8w {

struct Config {
  /** The launcher's name of the worker class, for READY's info. */
  std::string cls = "v8";
  int spin = 2000;
  uint64_t heap_max = 512ULL << 20;
  /** The DBA's npm packages (<package_dir>/js); empty: none. */
  std::string packages;
  /** Directories of ICU data files of the distribution's Node (/usr/share/node-22/icudata). */
  std::vector<std::string> icu_data;
  /** The largest host message the worker holds, in bytes; a larger one is discarded and failed
      where it belongs (§5.3). 0: whatever memory can be allocated. */
  uint64_t message_max = 0;
};

/** An Error of a RESULT (§7.4). */
struct ErrorInfo {
  /** "js", "" (the worker's own error) or a marker (MLE-Type, MLE-TimeZone, MLE-Option). */
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
  /** The error_id of the SQL_RESULT ERROR this error hands back (§10.8), or 0. */
  uint64_t sql_error_id = 0;
};

class Worker {
 public:
  /** Starts V8 and installs the worker's JavaScript. Mailbox::open has been called. */
  explicit Worker(const Config &config);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  /** READY, then requests until the host is gone (the control thread ends the process). */
  [[noreturn]] void run();

  /** Delivers an interrupt (EngineHooks::interrupt): any thread. */
  void terminate() { m_isolate->TerminateExecution(); }

  // Natives (api.js's and sql.js's N).
  void write(int stream, std::string_view text);
  void clear_console();
  /** N.sql(action, scope, id, text, params, flags, width, integerType, decimalType): one SQL
      message and its reply (§10), nested requests served while it waits. Everything guest code
      can influence is checked here; the reply as an array (see worker.cc). Throws (a V8
      exception, returns empty) on what cannot be sent. */
  v8::MaybeLocal<v8::Value> sql(const v8::FunctionCallbackInfo<v8::Value> &info);
  /** N.handBack(error, id, code, state, message): marks a SqlError as made from an SQL_RESULT
      ERROR, so that a routine that ends with it hands that error back (§10.8). */
  void hand_back(const v8::FunctionCallbackInfo<v8::Value> &info);
  /** N.state(): [depth, scope, routine schema, integer type, decimal type] of the request served. */
  v8::Local<v8::Value> state();
  /** $c.lib(schema, name), N.library: a database library's module namespace (USING). */
  v8::MaybeLocal<v8::Value> library_namespace(const std::string &schema, const std::string &name);
  /** N.readFile / N.isDir for api.js's require(): only below the DBA's package directory. */
  std::optional<std::string> package_file(const std::string &path);
  bool package_dir_exists(const std::string &path);
  const std::string &packages() const { return m_config.packages; }

  static Worker *current() { return s_current; }

 private:
  /** A routine's script: its name and source lines (line 1 without the wrapper), for frames. */
  struct Script {
    std::string name;
    int prefix = 0;
    std::vector<std::string> lines;
  };
  /** A prepared routine. A running activation shares it, so a nested PREPARE or DROP of its id
      does not take the code away from it (§8.3). */
  struct Routine {
    v8::Global<v8::Function> fn;
    std::vector<ParamType> params;
    std::vector<uint32_t> outs;  // parameter indexes of OUT and INOUT, ascending
    bool has_return = false;
    ParamType ret;
    bool async = false;
    bool eval = false;  // entry <eval>: the value as text
    int script_id = 0;
    /** The <eval> script, which fn runs (V8 has no handle that keeps a Script but a Global). */
    std::unique_ptr<v8::Global<v8::Script>> eval_script;
    /** The routine's schema (PREPARE). */
    std::string schema;
  };

  /** An SQL statement id that is not CLOSED (§10.1). */
  struct Statement {
    enum class State : uint8_t { kIdle, kRows, kNext };
    State state = State::kIdle;
    /** Opened by PREPARE: its home is IDLE. Else by EXECUTE: its home is CLOSED. */
    bool prepared = false;
    /** PASS_RESULT_TO_CLIENT, given when the id was opened. */
    bool to_client = false;
    /** The current result set: its columns' types and its total_rows. */
    std::vector<ParamType> columns;
    uint64_t total_rows = 0;
  };

  /** A request being served, at its depth. */
  struct Request {
    uint32_t op = 0;
    uint32_t depth = 0;
    /** A number of its own: sql.js keeps it in statement handles and per-call options. */
    uint64_t scope = 0;
    /** PREPARE and CALL: guest code of this request may send SQL and LIBRARY. */
    bool guest = false;
    uint64_t routine_id = 0;
    uint64_t library_epoch = 0;
    std::string schema;
    /** CALL: the code this activation started with. */
    std::shared_ptr<Routine> routine;
    /** The statement ids of this request: the worker counts them from 1. */
    uint64_t statement_ids = 0;
    std::unordered_map<uint64_t, Statement> statements;
    Request *outer = nullptr;
  };

  /** What a request ends with: the RESULT's status, Error and values. */
  struct Reply {
    wire::Status status = wire::Status::kOk;
    ErrorInfo error;
    /** The start of the RESULT's body: has_return, the return value, out_params. Empty: none. */
    std::vector<std::byte> values;
    /** V8 terminated the guest code: the status comes from the reason (handle()). */
    bool terminated = false;

    void fail(wire::Status s, std::string language, std::string message) {
      status = s;
      error = ErrorInfo{std::move(language), std::move(message), {}, 0, "", 0};
      values.clear();
    }
  };

  void handle(const common::Message &m);
  void too_large(const common::Message &m, Request &q, Reply *reply);
  void prepare(const common::Message &m, Request &q, Reply *reply);
  void call(const common::Message &m, Request &q, Reply *reply);
  void drop(const common::Message &m);
  void reset(const common::Message &m);
  void options(const common::Message &m, Reply *reply);
  void parse_library(const common::Message &m, Reply *reply);
  void drop_routine(uint64_t id);
  /** The buffer for a host message in chunks; nullptr if the worker cannot hold it. */
  std::byte *allocate(std::vector<std::byte> &buffer, size_t total);

  /** Compiles a routine; on a compile error *error says it (and the function is empty). */
  void compile(const std::string &name, const std::string &entry, const std::string &source, Routine &r,
               const std::vector<std::string> &param_names, const std::vector<uint8_t> &modes,
               const std::vector<std::string> &usings, std::string *error);
  /** A script of ours (routine, eval) compiled and run; registers it for frames. */
  v8::MaybeLocal<v8::Value> run_script(const std::string &text, const std::string &name, int prefix,
                                       int *script_id);

  /** The request's failure from what tc caught; false if execution was terminated instead. */
  bool failed(v8::TryCatch &tc, Reply *reply);
  ErrorInfo error_of(v8::Local<v8::Value> exception, v8::Local<v8::Message> message);
  std::string compile_error(v8::Local<v8::Message> message, const std::string &name, int prefix);
  /** True if a termination was requested and V8 has not acted on it yet; it is consumed. */
  bool termination_pending();
  /** Drops the microtasks that a failed request left: they would run in a later request. */
  void discard_microtasks();

  /** Posts the RESULT of q. */
  void post_result(const Request &q, Reply &reply);
  void stats(uint32_t state);

  // SQL (§10).
  /** The reply to the SQL message just sent, nested requests served before it. */
  common::Message sql_reply(std::vector<std::byte> &assembled);
  /** Checks the SQL_RESULT m that answers action for statement id against §10.1 to §10.3, moves
      the id to its next state, and returns the reply for sql.js. rows_sent: the rows of an
      EXECUTE_MANY. Empty and a TypeError thrown if a value has no JavaScript form. */
  v8::MaybeLocal<v8::Value> sql_result(Request &q, wire::SqlAction action, uint64_t id, uint8_t flags,
                                       uint32_t rows_sent, const common::Message &m, const Options &rules);
  /** Sends CLOSE for a statement whose SQL_RESULT the worker could not hold (§5.3). */
  void sql_close(Request &q, uint64_t id);
  /** Why guest code may not send SQL or LIBRARY now; nullptr if it may (§10.1, §10.6). */
  const char *host_unavailable() const;

  // Database libraries (modules.cc).
  /** The most recent body of a library the worker has, by schema and name. */
  struct Library {
    uint64_t version = 0;
    std::string language, body;
  };
  /** A LIBRARY_RESULT BODY or UNCHANGED, held for one routine id and library (§9.10). */
  struct Answer {
    uint64_t library_epoch = 0;
    uint64_t version = 0;
  };
  struct LibraryName {
    std::string schema, name;
    std::string key() const { return schema + '\0' + name; }
  };
  /** A library's module, compiled from the body of one version. */
  struct ModuleEntry {
    uint64_t version = 0;
    v8::Global<v8::Module> module;
    /** The libraries it imports statically. */
    std::vector<LibraryName> imports;
    /** Once instantiated: the versions of all libraries it reaches, which it is linked with. */
    std::string linked_with;
  };
  /** Resolves an import for the request served: the library under an answer that holds (LIBRARY
      when none does); nullptr and *why if the import fails. */
  const Library *lib_resolve(const LibraryName &lib, std::string *why);
  bool lib_compile(const LibraryName &lib, const Library &body, ModuleEntry &entry);
  /** The module of a library for the request served, with every library it reaches resolved and
      linked anew where a version changed; empty and an exception if an import fails. */
  v8::MaybeLocal<v8::Module> library_module(const LibraryName &lib);
  v8::MaybeLocal<v8::Module> compile_module(const std::string &text, const std::string &name, const std::string &path);
  v8::MaybeLocal<v8::Module> package_module(v8::Local<v8::Context> ctx, const std::string &spec, const std::string &from);
  v8::MaybeLocal<v8::Value> module_namespace(v8::Local<v8::Context> ctx, v8::Local<v8::Module> m);
  static v8::MaybeLocal<v8::Module> resolve(v8::Local<v8::Context> ctx, v8::Local<v8::String> specifier,
                                            v8::Local<v8::FixedArray> assertions, v8::Local<v8::Module> referrer);
  static v8::MaybeLocal<v8::Promise> dynamic_import(v8::Local<v8::Context> ctx, v8::Local<v8::Data> options,
                                                    v8::Local<v8::Value> resource, v8::Local<v8::String> specifier,
                                                    v8::Local<v8::FixedArray> assertions);

  std::map<std::string, Library> m_libraries;
  std::unordered_map<uint64_t, std::map<std::string, Answer>> m_answers;
  std::map<std::string, ModuleEntry> m_modules;
  std::map<std::string, v8::Global<v8::Module>> m_package_modules;
  std::unordered_map<int, std::string> m_module_paths;

  static inline Worker *s_current = nullptr;
  static size_t near_heap_limit(void *data, size_t current, size_t initial);

  Config m_config;
  std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
  std::unique_ptr<v8::Platform> m_platform;
  std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
  v8::Isolate *m_isolate = nullptr;
  v8::Global<v8::Context> m_context;
  v8::Global<v8::Object> m_api;      // api.js's helpers
  v8::Global<v8::Function> m_show;   // <eval>: the text of a value
  v8::Global<v8::Function> m_probe;  // an empty function (termination_pending)
  /** On a SqlError made from an SQL_RESULT ERROR: [error_id, call_seq, errno, sqlstate, message].
      Guest code cannot read or forge it. */
  v8::Global<v8::Private> m_handed;
  Options m_options;
  bool m_options_in_force = false;
  std::unique_ptr<Values> m_values;
  std::unordered_map<uint64_t, std::shared_ptr<Routine>> m_routines;
  std::unordered_map<int, Script> m_scripts;
  /** Console output no RESULT has carried yet (§9.4). */
  std::string m_out, m_err;
  bool m_cleared = false;
  bool m_tainted = false;
  /** The heap limit was reached (near_heap_limit): the request ends EXHAUSTED. */
  bool m_exhausted = false;
  /** call_seq of the top-level request being served or served last. */
  uint64_t m_seq = 0;
  /** The innermost request being served; nullptr between requests. */
  Request *m_request = nullptr;
  uint64_t m_scopes = 0;
  std::vector<std::byte> m_sql_buf;
  std::vector<std::byte> m_result_buf;
  uint32_t m_calls = 0;
  /** C++ is writing values into a message: a value's toString or toJSON runs guest code, which
      then may not send to the host (SQL, a library): no message may be posted between the moment
      one is being composed and its post (§4.1). */
  int m_converting = 0;
  struct Converting {
    explicit Converting(int &n) : n_(n) { ++n_; }
    ~Converting() { --n_; }
    Converting(const Converting &) = delete;
    Converting &operator=(const Converting &) = delete;
    int &n_;
  };
};

}  // namespace mle::v8w

#endif  // MLE_V8_WORKER_HPP
