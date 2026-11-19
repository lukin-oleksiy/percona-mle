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
  @file worker/v8/modules.cc

  ES modules in the V8 worker (exp design note 63): database libraries (CREATE LIBRARY ... LANGUAGE
  JAVASCRIPT or JS; USING, import of "/schema/name", import()) and the DBA's npm packages
  (import of a package name; require() is api.js's). The library rules are the JVM worker's
  (LibraryFileSystem.java; the CPython worker's libraries.py is the same port):

  * Epochs: library DDL makes mysqld prepare the routines again, and each PREPARE starts an epoch;
    within one, a library confirmed for a routine is used without asking mysqld again.
  * Loadsets and generations (independent review C17): an evaluated library keeps the modules it
    imported then. Every library has a loadset (the libraries with their hashes fetched in the
    top-level request that first evaluated it, and their loadsets); when a library is confirmed for
    a routine its loadset is confirmed too, and if one of those changed or is refused a new
    generation starts: every library is evaluated again at its next use.
*/

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "worker.hpp"

namespace mle::v8w {

namespace w = mle::wire;
using common::Mailbox;
using common::Message;
using v8::Local;
using v8::Value;

namespace {
constexpr uint64_t kStampMix = 0x9E3779B97F4A7C15ULL;

/** "/schema/name" (also "schema/name", "schema.name") split; false if it is not one. */
bool library_path(std::string_view s, std::string *schema, std::string *name) {
  std::string p(s);
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  size_t sep = p.find('/');
  if (sep == std::string::npos) sep = p.find('.');
  if (sep == std::string::npos || sep == 0 || sep + 1 >= p.size()) return false;
  *schema = p.substr(0, sep);
  *name = p.substr(sep + 1);
  return name->find('/') == std::string::npos;
}
}  // namespace

uint64_t Worker::Library::stamp() const { return generation == 0 ? hash : hash ^ (generation * kStampMix); }

void Worker::libraries_revalidate() {
  ++m_lib_epoch;
  m_lib_confirmed.clear();
}

void Worker::lib_touch(const std::string &key, const Library &lib) {
  for (const auto &s : m_lib_open) {
    if (s == lib.loadset) continue;
    (*s)[key] = lib.hash;
    for (const auto &[k, h] : *lib.loadset) (*s)[k] = h;
  }
}

Worker::Library &Worker::lib_renewed(const std::string &key, const Library &lib) {
  Library n = lib;
  n.generation = m_lib_generation;
  n.loadset = std::make_shared<std::map<std::string, uint64_t>>();
  Library &kept = m_libraries[key] = std::move(n);
  lib_touch(key, kept);
  m_lib_open.push_back(kept.loadset);
  return kept;
}

void Worker::lib_new_generation() {
  ++m_lib_generation;
  m_lib_confirmed.clear();
}

void Worker::lib_confirm_loadset(const Library &lib) {
  const auto loadset = *lib.loadset;  // a copy: fetch() changes the maps
  for (const auto &[key, h] : loadset) {
    std::string schema, name;
    if (!library_path(key, &schema, &name)) continue;
    const Library *l = lib_fetch(schema, name, nullptr);
    if (l == nullptr) return;  // dropped, or not for this routine: fetch started a generation
    if (l->hash != h) {
      lib_new_generation();
      return;
    }
  }
}

const Worker::Library *Worker::lib_fetch(const std::string &schema, const std::string &name, std::string *why) {
  const std::string key = schema + "/" + name;
  const auto known_it = m_libraries.find(key);
  const Library *known = known_it == m_libraries.end() ? nullptr : &known_it->second;
  const std::string confirmation = std::to_string(m_routine) + ":" + key;
  if (known != nullptr && known->checked == m_lib_epoch && m_lib_confirmed.count(confirmation) != 0) {
    if (known->generation != m_lib_generation) return &lib_renewed(key, *known);
    if (!m_lib_open.empty()) lib_touch(key, *known);
    return known;
  }
  // LIBRARY: the body from mysqld (protocol.h kLibrary), unless the worker has the current one.
  std::vector<std::byte> msg(16 + schema.size() + name.size() + 8);
  Writer wr(msg);
  if (!(wr.str(schema) && wr.str(name) && wr.put<uint64_t>(known == nullptr ? 0 : known->hash))) {
    if (why != nullptr) *why = "a library name too long";
    return nullptr;
  }
  Mailbox::send(uint32_t(w::Op::kLibrary), uint32_t(w::Status::kOk), m_depth, m_seq, std::span(msg).first(wr.size()));
  std::vector<std::byte> assembled;
  const Message m = Mailbox::receive([&assembled](size_t total) {
    assembled.resize(total);
    return assembled.data();
  });
  if (m.op != uint32_t(w::Op::kLibraryResult) || m.depth != m_depth) {
    std::fprintf(stderr, "mle_v8_worker: fatal: unexpected message %u while loading a library\n", m.op);
    _exit(1);
  }
  Reader r(m.body, 0);
  const auto status = static_cast<w::LibraryStatus>(r.get<uint8_t>());
  const std::string language(r.str()), body(r.str());
  const auto hash = r.get<uint64_t>();
  const std::string message(r.str());
  if (status == w::LibraryStatus::kUnchanged && known != nullptr) {
    Library lib = *known;
    lib.checked = m_lib_epoch;
    Library &kept = m_libraries[key] = lib;
    m_lib_confirmed.insert(confirmation);
    if (kept.generation == m_lib_generation) {
      lib_touch(key, kept);
      lib_confirm_loadset(kept);
    }
    const auto again = m_libraries.find(key);  // the confirmation may have changed the maps
    if (again == m_libraries.end()) return nullptr;
    return again->second.generation != m_lib_generation ? &lib_renewed(key, again->second) : &again->second;
  }
  if (status == w::LibraryStatus::kBody) {
    Library lib;
    lib.language = language;
    lib.body = body;
    lib.hash = hash;
    lib.checked = m_lib_epoch;
    lib.generation = m_lib_generation;
    lib.loadset = std::make_shared<std::map<std::string, uint64_t>>();
    const bool changed = known != nullptr;
    Library &kept = m_libraries[key] = std::move(lib);
    if (changed) m_lib_confirmed.clear();  // routines confirmed for the old body confirm again
    m_lib_confirmed.insert(confirmation);
    lib_touch(key, kept);
    m_lib_open.push_back(kept.loadset);  // a new body: evaluated from here on
    return &kept;
  }
  const bool had = known != nullptr;
  m_libraries.erase(key);
  if (had || !m_lib_open.empty()) lib_new_generation();
  if (why != nullptr) *why = message.empty() ? "the library " + schema + "." + name + " is not available" : message;
  return nullptr;
}

// ------------------------------------------------------------ modules

v8::MaybeLocal<v8::Module> Worker::compile_module(const std::string &text, const std::string &name,
                                                  const std::string &path) {
  v8::ScriptOrigin origin(v8_string(m_isolate, name), 0, 0, false, -1, Local<Value>(), false, false, true);
  v8::ScriptCompiler::Source src(v8_string(m_isolate, text), origin);
  Local<v8::Module> m;
  if (!v8::ScriptCompiler::CompileModule(m_isolate, &src).ToLocal(&m)) return {};
  m_module_paths[m->GetIdentityHash()] = path;
  return m;
}

v8::MaybeLocal<v8::Module> Worker::library_module(const std::string &schema, const std::string &name) {
  std::string why;
  const Library *lib = lib_fetch(schema, name, &why);
  if (lib == nullptr) {
    m_isolate->ThrowException(v8::Exception::Error(v8_string(m_isolate, why)));
    return {};
  }
  if (lib->language != "js") {
    const std::string lang = lib->language == "python" ? "Python" : lib->language == "java" ? "Java" : lib->language;
    m_isolate->ThrowException(v8::Exception::Error(
        v8_string(m_isolate, "the library " + schema + "." + name + " is a " + lang +
                                 " library: it needs the JVM worker (LANGUAGE JAVASCRIPT)")));
    return {};
  }
  const std::string key = schema + "/" + name;
  const uint64_t stamp = lib->stamp();
  if (const auto hit = m_modules.find(key); hit != m_modules.end() && hit->second.stamp == stamp)
    return hit->second.module.Get(m_isolate);
  // A new library (or version) is evaluated within the loadsets of this request.
  if (std::none_of(m_lib_open.begin(), m_lib_open.end(), [&](const auto &s) { return s == lib->loadset; }))
    m_lib_open.push_back(lib->loadset);
  Local<v8::Module> m;
  if (!compile_module(lib->body, schema + "." + name, "/" + key).ToLocal(&m)) return {};
  m_modules[key] = ModuleEntry{stamp, v8::Global<v8::Module>(m_isolate, m)};
  return m;
}

v8::MaybeLocal<v8::Module> Worker::resolve(Local<v8::Context> ctx, Local<v8::String> specifier,
                                           Local<v8::FixedArray>, Local<v8::Module> referrer) {
  Worker *self = current();
  const std::string spec = utf8(self->m_isolate, specifier);
  std::string schema, name;
  // "/schema/name": a database library; anything else: an npm package (api.js resolves it).
  if (spec.starts_with("/") && library_path(spec, &schema, &name)) return self->library_module(schema, name);
  std::string from;
  if (!referrer.IsEmpty())
    if (const auto p = self->m_module_paths.find(referrer->GetIdentityHash()); p != self->m_module_paths.end())
      from = p->second;
  return self->package_module(ctx, spec, from);
}

v8::MaybeLocal<v8::Module> Worker::package_module(Local<v8::Context> ctx, const std::string &spec,
                                                  const std::string &from) {
  // api.js's resolver: [path, kind ('esm' | 'cjs' | 'json'), source]; throws if not found.
  const Local<v8::Object> api = m_api.Get(m_isolate);
  Local<Value> fn;
  if (!api->Get(ctx, v8_string(m_isolate, "resolvePackage")).ToLocal(&fn) || !fn->IsFunction()) return {};
  // Relative names resolve from the importing package file (a library is no such file).
  const bool in_packages = !m_config.packages.empty() && from.starts_with(m_config.packages + "/");
  Local<Value> args[] = {v8_string(m_isolate, spec), v8_string(m_isolate, in_packages ? from : "")};
  Local<Value> found;
  if (!fn.As<v8::Function>()->Call(ctx, api, 2, args).ToLocal(&found) || !found->IsArray()) return {};
  const Local<v8::Array> a = found.As<v8::Array>();
  const std::string path = utf8(m_isolate, a->Get(ctx, 0).ToLocalChecked());
  const std::string kind = utf8(m_isolate, a->Get(ctx, 1).ToLocalChecked());
  if (const auto hit = m_package_modules.find(path); hit != m_package_modules.end()) return hit->second.Get(m_isolate);
  Local<v8::Module> m;
  if (kind == "esm") {
    if (!compile_module(utf8(m_isolate, a->Get(ctx, 2).ToLocalChecked()), path, path).ToLocal(&m)) return {};
  } else {
    // CommonJS and JSON: a module whose default export is require()'s value (Node's interop).
    std::vector<Local<v8::String>> names = {v8_string(m_isolate, "default")};
    m = v8::Module::CreateSyntheticModule(
        m_isolate, v8_string(m_isolate, path), v8::MemorySpan<const Local<v8::String>>(names.data(), names.size()),
        [](Local<v8::Context> c, Local<v8::Module> mod) -> v8::MaybeLocal<Value> {
          Worker *self = current();
          v8::Isolate *iso = self->m_isolate;
          const auto p = self->m_module_paths.find(mod->GetIdentityHash());
          const Local<v8::Object> api = self->m_api.Get(iso);
          Local<Value> req;
          if (p == self->m_module_paths.end() || !api->Get(c, v8_string(iso, "requirePath")).ToLocal(&req))
            return {};
          Local<Value> arg = v8_string(iso, p->second);
          Local<Value> exports;
          if (!req.As<v8::Function>()->Call(c, api, 1, &arg).ToLocal(&exports)) return {};
          if (mod->SetSyntheticModuleExport(iso, v8_string(iso, "default"), exports).IsNothing()) return {};
          return v8::Undefined(iso);
        });
    m_module_paths[m->GetIdentityHash()] = path;
  }
  m_package_modules[path] = v8::Global<v8::Module>(m_isolate, m);
  return m;
}

v8::MaybeLocal<Value> Worker::module_namespace(Local<v8::Context> ctx, Local<v8::Module> m) {
  if (m->GetStatus() < v8::Module::kInstantiated && m->InstantiateModule(ctx, resolve).IsNothing()) return {};
  if (m->GetStatus() < v8::Module::kEvaluated) {
    Local<Value> done;
    if (!m->Evaluate(ctx).ToLocal(&done)) return {};
    // Top-level await: the jobs run now (an ES module evaluates to a promise).
    m_isolate->PerformMicrotaskCheckpoint();
    if (done->IsPromise() && done.As<v8::Promise>()->State() == v8::Promise::kRejected) {
      m_isolate->ThrowException(done.As<v8::Promise>()->Result());
      return {};
    }
  }
  if (m->GetStatus() == v8::Module::kErrored) {
    m_isolate->ThrowException(m->GetException());
    return {};
  }
  return m->GetModuleNamespace();
}

v8::MaybeLocal<Value> Worker::library_namespace(const std::string &schema, const std::string &name) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  Local<v8::Module> m;
  if (!library_module(schema, name).ToLocal(&m)) return {};
  return module_namespace(ctx, m);
}

v8::MaybeLocal<v8::Promise> Worker::dynamic_import(Local<v8::Context> ctx, Local<v8::Data>, Local<Value>,
                                                   Local<v8::String> specifier, Local<v8::FixedArray>) {
  Worker *self = current();
  v8::Isolate *iso = self->m_isolate;
  Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return {};
  v8::TryCatch tc(iso);
  Local<v8::Module> m;
  Local<Value> ns;
  const bool ok = resolve(ctx, specifier, Local<v8::FixedArray>(), Local<v8::Module>()).ToLocal(&m) &&
                  self->module_namespace(ctx, m).ToLocal(&ns);
  if (tc.HasTerminated()) {
    tc.ReThrow();
    return {};
  }
  if (ok)
    (void)resolver->Resolve(ctx, ns);
  else
    (void)resolver->Reject(ctx, tc.HasCaught() ? tc.Exception() : Local<Value>(v8::Undefined(iso)));
  return resolver->GetPromise();
}

// ------------------------------------------------------------ the DBA's files

std::optional<std::string> Worker::package_file(const std::string &path) {
  // Only below <package_dir>/js, and no ".." (Landlock allows only that directory anyway).
  if (m_config.packages.empty() || !path.starts_with(m_config.packages + "/") ||
      path.find("/../") != std::string::npos || path.ends_with("/.."))
    return std::nullopt;
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool Worker::package_dir_exists(const std::string &path) {
  if (m_config.packages.empty() || !path.starts_with(m_config.packages) || path.find("/../") != std::string::npos)
    return false;
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace mle::v8w
