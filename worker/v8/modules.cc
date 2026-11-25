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

  ES modules in the V8 worker: database libraries (CREATE LIBRARY; USING, import of "/schema/name",
  import()) by the rules of docs/protocol.md §9.10, and the DBA's npm packages (import of a package
  name; require() is api.js's).

  * Answers. An import is resolved for the request being served: the worker must hold a
    LIBRARY_RESULT (BODY or UNCHANGED) that was given for this routine id under this request's
    library_epoch, and asks with LIBRARY when it does not. One answer per routine id and library;
    the answers of an id go at its PREPARE, its DROP and at RESET.
  * Bodies. The most recent body of each library is kept with the host's version token, which is
    the known_version of the next LIBRARY for it.
  * Modules. V8 links a module once, with the modules it imports. So every import, also one V8
    would take from the modules it has, first resolves the library and every library it reaches
    through static imports; a module whose own version, or the version of anything it reaches,
    differs from what it was linked with is compiled and linked anew. Guest code that keeps a
    module namespace it imported earlier keeps that one (its own reference).
*/

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "worker.hpp"

namespace mle::v8w {

namespace w = mle::wire;
using common::Mailbox;
using common::Message;
using v8::Local;
using v8::Value;

namespace {

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

/** A library's schema or name as LIBRARY may carry it: 1 to 256 bytes without U+0000 (§9.10). */
bool library_name(std::string_view s) {
  return !s.empty() && s.size() <= w::kMaxLibraryName && s.find('\0') == std::string_view::npos;
}

}  // namespace

const Worker::Library *Worker::lib_resolve(const LibraryName &lib, std::string *why) {
  Request &q = *m_request;
  const std::string key = lib.key();
  const auto known = m_libraries.find(key);
  std::map<std::string, Answer> &answers = m_answers[q.routine_id];
  if (const auto held = answers.find(key);
      held != answers.end() && held->second.library_epoch == q.library_epoch && known != m_libraries.end() &&
      known->second.version == held->second.version)
    return &known->second;
  // No answer holds for this request, or its body is no longer the one the worker has: LIBRARY.
  const uint64_t known_version = known == m_libraries.end() ? 0 : known->second.version;
  std::array<std::byte, 16 + 2 * w::kMaxLibraryName> msg;
  Writer wr(msg);
  if (!library_name(lib.schema) || !library_name(lib.name) ||
      !(wr.str(lib.schema) && wr.str(lib.name) && wr.put(known_version))) {
    *why = "a library's schema and name must each have 1 to " + std::to_string(w::kMaxLibraryName) +
           " bytes and no U+0000";
    return nullptr;
  }
  Mailbox::send(uint32_t(w::Op::kLibrary), 0, q.depth, m_seq, wr.written());
  std::vector<std::byte> assembled;
  const Message m = Mailbox::receive([&](size_t total) { return allocate(assembled, total); });
  if (m.discarded) {
    // §5.3: as REFUSED. No answer is held, and a held one is unchanged.
    *why = "the library " + lib.schema + "." + lib.name + " is larger than this MLE worker can hold";
    return nullptr;
  }
  Reader r(m.body);
  const auto status = r.get<uint8_t>();
  const std::string_view language = r.str(), body = r.str();
  const auto version = r.get<uint64_t>();
  const std::string_view message = r.str();
  uint64_t held = version;
  switch (status) {
    case uint8_t(w::LibraryStatus::kRefused):
      *why = message.empty() ? "the library " + lib.schema + "." + lib.name + " is not available" : std::string(message);
      return nullptr;
    case uint8_t(w::LibraryStatus::kUnchanged):
      // The body the worker holds, under the version it named (§1: UNCHANGED against a body it holds).
      if (known_version == 0) protocol_failure("a LIBRARY_RESULT UNCHANGED for a library the worker does not hold");
      held = known_version;
      break;
    case uint8_t(w::LibraryStatus::kBody):
      m_libraries[key] = Library{version, std::string(language), std::string(body)};
      break;
    default:
      protocol_failure("an unknown LIBRARY_RESULT status");
  }
  answers[key] = Answer{q.library_epoch, held};
  return &m_libraries[key];
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

bool Worker::lib_compile(const LibraryName &lib, const Library &body, ModuleEntry &entry) {
  const Local<v8::Context> ctx = m_context.Get(m_isolate);
  if (!entry.module.IsEmpty()) m_module_paths.erase(entry.module.Get(m_isolate)->GetIdentityHash());
  entry = ModuleEntry();
  Local<v8::Module> m;
  try {
    if (!compile_module(body.body, lib.schema + "." + lib.name, "/" + lib.schema + "/" + lib.name).ToLocal(&m))
      return false;
  } catch (const ConversionError &e) {  // a body longer than a JavaScript string can be
    m_isolate->ThrowException(v8::Exception::RangeError(v8_string(m_isolate, e.message)));
    return false;
  }
  // The libraries it imports statically; any other specifier is a package's.
  const Local<v8::FixedArray> requests = m->GetModuleRequests();
  for (int i = 0; i < requests->Length(); i++) {
    const Local<v8::String> specifier = requests->Get(ctx, i).As<v8::ModuleRequest>()->GetSpecifier();
    const std::optional<std::string> spec = utf8_exact(m_isolate, specifier);
    LibraryName import;
    if (spec && spec->starts_with("/") && library_path(*spec, &import.schema, &import.name))
      entry.imports.push_back(std::move(import));
  }
  entry.version = body.version;
  entry.module.Reset(m_isolate, m);
  return true;
}

v8::MaybeLocal<v8::Module> Worker::library_module(const LibraryName &root) {
  const auto fail = [this](const std::string &text) {
    m_isolate->ThrowException(v8::Exception::Error(v8_string(m_isolate, text)));
    return v8::MaybeLocal<v8::Module>();
  };
  // Imports come only from the guest code of the request being served (§10.1).
  if (const char *why = host_unavailable(); why != nullptr) return fail(why);
  // Every library the import reaches is resolved for this request, each once.
  std::set<std::string> reached;
  std::vector<LibraryName> todo{root};
  while (!todo.empty()) {
    const LibraryName lib = std::move(todo.back());
    todo.pop_back();
    const std::string key = lib.key();
    if (reached.contains(key)) continue;
    std::string why;
    const Library *body = lib_resolve(lib, &why);
    if (body == nullptr) return fail(why);
    if (body->language != "js") {
      const std::string language =
          body->language == "python" ? "Python" : body->language == "java" ? "Java" : body->language;
      return fail("the library " + lib.schema + "." + lib.name + " is a " + language +
                  " library: the V8 worker runs JavaScript libraries only");
    }
    ModuleEntry &entry = m_modules[key];
    // A module whose evaluation failed (an exception, or an interrupt: V8 then records null) is
    // compiled anew, as a fresh import: kept, it would throw that exception on every import.
    const bool errored = !entry.module.IsEmpty() && entry.module.Get(m_isolate)->GetStatus() == v8::Module::kErrored;
    if ((entry.module.IsEmpty() || entry.version != body->version || errored) && !lib_compile(lib, *body, entry))
      return {};
    reached.insert(key);
    for (const LibraryName &import : entry.imports) todo.push_back(import);
  }
  // What each module reaches, itself included, with the versions: a module that is linked with
  // other versions is compiled anew, and V8 links it when it is instantiated.
  for (const std::string &key : reached) {
    std::map<std::string, uint64_t> closure;
    std::vector<std::string> walk{key};
    while (!walk.empty()) {
      const std::string k = std::move(walk.back());
      walk.pop_back();
      const ModuleEntry &e = m_modules[k];
      if (!closure.emplace(k, e.version).second) continue;
      for (const LibraryName &import : e.imports) walk.push_back(import.key());
    }
    std::string linked_with;
    for (const auto &[k, version] : closure) linked_with += k + '\0' + std::to_string(version) + '\0';
    ModuleEntry &entry = m_modules[key];
    const bool linked = entry.module.Get(m_isolate)->GetStatus() >= v8::Module::kInstantiated;
    if (linked && entry.linked_with != linked_with) {
      const size_t sep = key.find('\0');
      if (!lib_compile(LibraryName{key.substr(0, sep), key.substr(sep + 1)}, m_libraries[key], entry)) return {};
    }
    entry.linked_with = linked_with;
  }
  return m_modules[root.key()].module.Get(m_isolate);
}

v8::MaybeLocal<v8::Module> Worker::resolve(Local<v8::Context> ctx, Local<v8::String> specifier,
                                           Local<v8::FixedArray>, Local<v8::Module> referrer) {
  Worker *self = current();
  const std::optional<std::string> spec = utf8_exact(self->m_isolate, specifier);
  if (!spec) {
    self->m_isolate->ThrowException(v8::Exception::TypeError(
        v8_string(self->m_isolate, "a module name must be well-formed Unicode (no unpaired surrogate)")));
    return {};
  }
  std::string from;
  if (!referrer.IsEmpty())
    if (const auto p = self->m_module_paths.find(referrer->GetIdentityHash()); p != self->m_module_paths.end())
      from = p->second;
  LibraryName lib;
  // "/schema/name": a database library; anything else: an npm package (api.js resolves it).
  if (spec->starts_with("/") && library_path(*spec, &lib.schema, &lib.name)) {
    // A library that imports it was resolved with everything it reaches (library_module): this is
    // the module of that resolution. A package's module comes here with nothing resolved.
    const bool from_package = !self->m_config.packages.empty() && from.starts_with(self->m_config.packages + "/");
    if (const auto resolved = self->m_modules.find(lib.key());
        !from_package && resolved != self->m_modules.end() && !resolved->second.module.IsEmpty())
      return resolved->second.module.Get(self->m_isolate);
    return self->library_module(lib);
  }
  return self->package_module(ctx, *spec, from);
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
  // Guest code that escapes its wrapper can replace the resolver: its array may have getters.
  const Local<v8::Array> a = found.As<v8::Array>();
  Local<Value> path_v, kind_v;
  if (!a->Get(ctx, 0).ToLocal(&path_v) || !a->Get(ctx, 1).ToLocal(&kind_v)) return {};
  const std::string path = utf8(m_isolate, path_v);
  const std::string kind = utf8(m_isolate, kind_v);
  if (const auto hit = m_package_modules.find(path); hit != m_package_modules.end()) {
    const Local<v8::Module> cached = hit->second.Get(m_isolate);
    if (cached->GetStatus() != v8::Module::kErrored) return cached;
    m_module_paths.erase(cached->GetIdentityHash());  // failed: compiled anew, as a library is
    m_package_modules.erase(hit);
  }
  Local<v8::Module> m;
  if (kind == "esm") {
    Local<Value> source;
    if (!a->Get(ctx, 2).ToLocal(&source) || !compile_module(utf8(m_isolate, source), path, path).ToLocal(&m))
      return {};
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
  if (!library_module(LibraryName{schema, name}).ToLocal(&m)) return {};
  return module_namespace(ctx, m);
}

v8::MaybeLocal<v8::Promise> Worker::dynamic_import(Local<v8::Context> ctx, Local<v8::Data>, Local<Value> resource,
                                                   Local<v8::String> specifier, Local<v8::FixedArray>) {
  Worker *self = current();
  v8::Isolate *iso = self->m_isolate;
  Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return {};
  v8::TryCatch tc(iso);
  Local<v8::Module> m;
  Local<Value> ns;
  bool ok = false;
  // A module name is a name: it is not repaired (§7.1).
  const std::optional<std::string> spec = utf8_exact(iso, specifier);
  if (!spec) {
    iso->ThrowException(v8::Exception::TypeError(
        v8_string(iso, "a module name must be well-formed Unicode (no unpaired surrogate)")));
  } else {
    // The importer: its resource name, a package module's path (relative names and node_modules
    // resolve from it, as for a static import; package_module ignores any other name).
    LibraryName lib;
    try {
      ok = (spec->starts_with("/") && library_path(*spec, &lib.schema, &lib.name)
                ? self->library_module(lib)
                : self->package_module(ctx, *spec, resource->IsString() ? utf8(iso, resource) : std::string()))
               .ToLocal(&m) &&
           self->module_namespace(ctx, m).ToLocal(&ns);
    } catch (const ConversionError &e) {
      iso->ThrowException(v8::Exception::RangeError(v8_string(iso, e.message)));
    }
  }
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
