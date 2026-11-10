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

#include "routine.h"

#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysql/components/services/mysql_string.h>
#include <mysqld_error.h>

#include "mle_component.h"

extern REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_metadata_query);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_import_metadata_query);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_argument_metadata_query);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_metadata_query);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_int);
extern REQUIRES_SERVICE_PLACEHOLDER(
    mysql_stored_program_runtime_argument_unsigned_int);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_float);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_string);
extern REQUIRES_SERVICE_PLACEHOLDER(
    mysql_stored_program_runtime_argument_string_charset);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_null);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_int);
extern REQUIRES_SERVICE_PLACEHOLDER(
    mysql_stored_program_return_value_unsigned_int);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_float);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_string);
extern REQUIRES_SERVICE_PLACEHOLDER(
    mysql_stored_program_return_value_string_charset);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_null);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_charset);

namespace mle_component {

using mle::Tag;
using mle::Value;
using mle::wire::ParamMode;

namespace {

std::atomic<uint64_t> g_next_id{0};
std::atomic<long> g_alive{0};

const char *worker_language(unsigned bit) {
  switch (bit) {
    case kJavaScript:
      return "js";
    case kPython:
      return "python";
    default:
      return "java";
  }
}

// ------------------------------------------------------------- names

const std::set<std::string> &reserved(unsigned language) {
  // Oracle reserves mysql, console and graal; JavaScript keywords and the
  // names the worker's wrapper uses ($c, $G, $r, $a0..) cannot be
  // parameters either.
  static const std::set<std::string> js = {
      "mysql",      "console",  "graal",    "globalThis", "arguments",
      "eval",       "break",    "case",     "catch",      "class",
      "const",      "continue", "debugger", "default",    "delete",
      "do",         "else",     "enum",     "export",     "extends",
      "false",      "finally",  "for",      "function",   "if",
      "import",     "in",       "instanceof", "new",      "null",
      "return",     "super",    "switch",   "this",       "throw",
      "true",       "try",      "typeof",   "var",        "void",
      "while",      "with",     "yield",    "let",        "static",
      "implements", "interface", "package", "private",    "protected",
      "public",     "await",    "undefined", "$c",        "$G",
      "$r"};
  static const std::set<std::string> python = {
      "False", "None",     "True",   "and",    "as",       "assert",
      "async", "await",    "break",  "class",  "continue", "def",
      "del",   "elif",     "else",   "except", "finally",  "for",
      "from",  "global",   "if",     "import", "in",       "is",
      "lambda", "nonlocal", "not",   "or",     "pass",     "raise",
      "return", "try",     "while",  "with",   "yield",    "Mle",
      "__mle_r", "__mle_body", "__mle_routine"};
  static const std::set<std::string> java = {
      "abstract", "assert",     "boolean",   "break",      "byte",
      "case",     "catch",      "char",      "class",      "const",
      "continue", "default",    "do",        "double",     "else",
      "enum",     "extends",    "final",     "finally",    "float",
      "for",      "goto",       "if",        "implements", "import",
      "instanceof", "int",      "interface", "long",       "native",
      "new",      "package",    "private",   "protected",  "public",
      "return",   "short",      "static",    "strictfp",   "super",
      "switch",   "synchronized", "this",    "throw",      "throws",
      "transient", "try",       "void",      "volatile",   "while",
      "true",     "false",      "null",      "var",        "yield",
      "record",   "_"};
  return language == kJavaScript ? js : language == kPython ? python : java;
}

/** A parameter name the language accepts as a variable. */
bool valid_name(const std::string &name, unsigned language) {
  if (name.empty() || reserved(language).count(name) != 0) return false;
  if (language == kJavaScript && name.size() > 2 && name[0] == '$' &&
      name[1] == 'a' && isdigit(static_cast<unsigned char>(name[2])))
    return false;  // $a0, $a1, ...: the wrapper's argument names
  const bool dollar = language != kPython;
  for (size_t i = 0; i < name.size(); i++) {
    const auto c = static_cast<unsigned char>(name[i]);
    const bool ok = isalpha(c) || c == '_' || (dollar && c == '$') ||
                    (i > 0 && isdigit(c));
    if (!ok) return false;
  }
  return true;
}

// ------------------------------------------------------------- types

struct TypeQuery {
  stored_program_handle sp;
  int index;  // -1: the return value

  template <typename T>
  bool get(const char *key, T *value) const {
    if (index < 0)
      return mysql_service_mysql_stored_program_return_metadata_query->get(
          sp, key, value);
    return mysql_service_mysql_stored_program_argument_metadata_query->get(
        sp, static_cast<uint16_t>(index), key, value);
  }
};

}  // namespace

const char *sql_type_name(uint64_t t) {
  switch (t) {
    case MYSQL_SP_ARG_TYPE_TINY:
    case MYSQL_SP_ARG_TYPE_BOOL:
      return "TINYINT";
    case MYSQL_SP_ARG_TYPE_SHORT:
      return "SMALLINT";
    case MYSQL_SP_ARG_TYPE_INT24:
      return "MEDIUMINT";
    case MYSQL_SP_ARG_TYPE_LONG:
      return "INT";
    case MYSQL_SP_ARG_TYPE_LONGLONG:
      return "BIGINT";
    case MYSQL_SP_ARG_TYPE_YEAR:
      return "YEAR";
    case MYSQL_SP_ARG_TYPE_BIT:
      return "BIT";
    case MYSQL_SP_ARG_TYPE_FLOAT:
      return "FLOAT";
    case MYSQL_SP_ARG_TYPE_DOUBLE:
      return "DOUBLE";
    case MYSQL_SP_ARG_TYPE_NEWDECIMAL:
      return "DECIMAL";
    case MYSQL_SP_ARG_TYPE_NEWDATE:
      return "DATE";
    case MYSQL_SP_ARG_TYPE_TIME2:
      return "TIME";
    case MYSQL_SP_ARG_TYPE_DATETIME2:
      return "DATETIME";
    case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
      return "TIMESTAMP";
    case MYSQL_SP_ARG_TYPE_JSON:
      return "JSON";
    case MYSQL_SP_ARG_TYPE_VARCHAR:
      return "VARCHAR";
    case MYSQL_SP_ARG_TYPE_STRING:
      return "CHAR";
    case MYSQL_SP_ARG_TYPE_TINY_BLOB:
    case MYSQL_SP_ARG_TYPE_MEDIUM_BLOB:
    case MYSQL_SP_ARG_TYPE_BLOB:
    case MYSQL_SP_ARG_TYPE_LONG_BLOB:
      return "BLOB";
    case MYSQL_SP_ARG_TYPE_ENUM:
      return "ENUM";
    case MYSQL_SP_ARG_TYPE_SET:
      return "SET";
    case MYSQL_SP_ARG_TYPE_VECTOR:
      return "VECTOR";
    case MYSQL_SP_ARG_TYPE_GEOMETRY:
      return "GEOMETRY";
    default:
      return "";
  }
}

namespace {

/** Fills info from the metadata; false after reporting an unsupported
    type. what: "parameter 'a'" or "return value". */
bool describe(const TypeQuery &q, const std::string &what, ParamInfo *info) {
  uint64_t t = 0;
  bool is_signed = true;
  if (q.get("sql_type", &t)) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             ("MLE-Type> Cannot get the type of the " + what).c_str());
    return false;
  }
  (void)q.get("is_signed", &is_signed);
  auto unsupported = [&](const std::string &why) {
    my_error(ER_LANGUAGE_COMPONENT_UNSUPPORTED_TYPE, MYF(0), "MLE-Type",
             (why + " (" + what + ")").c_str());
    return false;
  };
  mle::ParamType &p = info->type;
  p.type_name = sql_type_name(t);
  p.is_unsigned = !is_signed;
  switch (t) {
    case MYSQL_SP_ARG_TYPE_TINY:
    case MYSQL_SP_ARG_TYPE_BOOL:
    case MYSQL_SP_ARG_TYPE_SHORT:
    case MYSQL_SP_ARG_TYPE_INT24:
    case MYSQL_SP_ARG_TYPE_LONG:
      p.tag = Tag::kI64;
      info->kind = is_signed ? Kind::kInt : Kind::kUInt;
      return true;
    case MYSQL_SP_ARG_TYPE_YEAR:
      p.tag = Tag::kI64;
      p.is_unsigned = false;
      info->kind = Kind::kInt;
      return true;
    case MYSQL_SP_ARG_TYPE_LONGLONG:
      p.tag = is_signed ? Tag::kI64 : Tag::kU64;
      info->kind = is_signed ? Kind::kInt : Kind::kBigUInt;
      return true;
    case MYSQL_SP_ARG_TYPE_BIT: {
      size_t bits = 64;
      (void)q.get("max_display_length", &bits);
      p.tag = Tag::kU64;
      p.is_unsigned = true;
      p.length = static_cast<uint32_t>(bits);
      info->kind = Kind::kBigUInt;
      return true;
    }
    case MYSQL_SP_ARG_TYPE_FLOAT:
      p.tag = Tag::kF32;
      info->kind = Kind::kFloat;
      return true;
    case MYSQL_SP_ARG_TYPE_DOUBLE:
      p.tag = Tag::kF64;
      info->kind = Kind::kDouble;
      return true;
    case MYSQL_SP_ARG_TYPE_NEWDECIMAL:
      p.tag = Tag::kDecimal;
      info->kind = Kind::kDecimal;
      return true;
    case MYSQL_SP_ARG_TYPE_NEWDATE:
      p.tag = Tag::kDate;
      info->kind = Kind::kDate;
      return true;
    case MYSQL_SP_ARG_TYPE_TIME2:
      p.tag = Tag::kTime;
      info->kind = Kind::kTime;
      return true;
    case MYSQL_SP_ARG_TYPE_DATETIME2:
      p.tag = Tag::kDatetime;
      info->kind = Kind::kDatetime;
      return true;
    case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
      p.tag = Tag::kTimestamp;
      info->kind = Kind::kTimestamp;
      return true;
    case MYSQL_SP_ARG_TYPE_JSON:
      p.tag = Tag::kJson;
      info->kind = Kind::kJson;
      return true;
    case MYSQL_SP_ARG_TYPE_VECTOR: {
      uint64_t bytes = 0;
      (void)q.get("byte_length", &bytes);
      p.tag = Tag::kVector;
      p.length = static_cast<uint32_t>(bytes / 4);
      info->kind = Kind::kVector;
      return true;
    }
    case MYSQL_SP_ARG_TYPE_VARCHAR:
    case MYSQL_SP_ARG_TYPE_STRING:
    case MYSQL_SP_ARG_TYPE_TINY_BLOB:
    case MYSQL_SP_ARG_TYPE_MEDIUM_BLOB:
    case MYSQL_SP_ARG_TYPE_BLOB:
    case MYSQL_SP_ARG_TYPE_LONG_BLOB:
    case MYSQL_SP_ARG_TYPE_ENUM:
    case MYSQL_SP_ARG_TYPE_SET: {
      const char *charset = nullptr;
      (void)q.get("charset", &charset);
      const std::string cs = charset == nullptr ? "" : charset;
      uint64_t chars = 0;
      (void)q.get("char_length", &chars);
      p.length = static_cast<uint32_t>(chars);
      if (cs == "utf8mb4") {
        p.tag = Tag::kString;
        info->kind = Kind::kString;
        if (p.type_name == "BLOB") p.type_name = "TEXT";
        return true;
      }
      if (cs == "binary" && t != MYSQL_SP_ARG_TYPE_ENUM &&
          t != MYSQL_SP_ARG_TYPE_SET) {
        p.tag = Tag::kBytes;
        info->kind = Kind::kBytes;
        if (p.type_name == "VARCHAR") p.type_name = "VARBINARY";
        if (p.type_name == "CHAR") p.type_name = "BINARY";
        return true;
      }
      return unsupported("The character set " + cs +
                         " is not supported; use utf8mb4 or binary");
    }
    default:
      return unsupported(std::string("The type ") +
                         (*sql_type_name(t) ? sql_type_name(t) : "?") +
                         " is not supported");
  }
}

// ------------------------------------------------------------- temporals

/** "2024-01-02", "2024-01-02 03:04:05.123", "-838:59:59.5" */
bool parse_temporal(Kind kind, const char *s, size_t n, mle::Temporal *t) {
  std::string text(s, n);
  unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  char frac[16] = "";
  if (kind == Kind::kTime) {
    const char *p = text.c_str();
    if (*p == '-') {
      t->negative = true;
      p++;
    }
    if (sscanf(p, "%u:%u:%u.%15[0-9]", &h, &mi, &sec, frac) < 3) return false;
  } else if (kind == Kind::kDate) {
    if (sscanf(text.c_str(), "%u-%u-%u", &y, &mo, &d) != 3) return false;
  } else {
    if (sscanf(text.c_str(), "%u-%u-%u %u:%u:%u.%15[0-9]", &y, &mo, &d, &h,
               &mi, &sec, frac) < 6)
      return false;
  }
  t->year = static_cast<uint16_t>(y);
  t->month = static_cast<uint8_t>(mo);
  t->day = static_cast<uint8_t>(d);
  t->hour = h;
  t->minute = static_cast<uint8_t>(mi);
  t->second = static_cast<uint8_t>(sec);
  std::string f = frac;
  f.resize(6, '0');
  t->microsecond = static_cast<uint32_t>(strtoul(f.c_str(), nullptr, 10));
  return true;
}

std::string format_temporal(Tag tag, const mle::Temporal &t) {
  char b[64];
  if (tag == Tag::kTime)
    snprintf(b, sizeof b, "%s%u:%02u:%02u.%06u", t.negative ? "-" : "", t.hour,
             t.minute, t.second, t.microsecond);
  else if (tag == Tag::kDate)
    snprintf(b, sizeof b, "%04u-%02u-%02u", t.year, t.month, t.day);
  else
    snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u:%02u.%06u", t.year, t.month,
             t.day, t.hour, t.minute, t.second, t.microsecond);
  return b;
}

CHARSET_INFO_h binary_charset() {
  static CHARSET_INFO_h cs = mysql_service_mysql_charset->get("binary");
  return cs;
}

// ------------------------------------------------------------- storing

/** Sets a return value (index < 0) or an OUT parameter. */
struct Target {
  int index;

  bool null() const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_null->set(
                     nullptr)
               : mysql_service_mysql_stored_program_runtime_argument_null->set(
                     nullptr, static_cast<uint16_t>(index));
  }
  bool i64(int64_t v) const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_int->set(
                     nullptr, v)
               : mysql_service_mysql_stored_program_runtime_argument_int->set(
                     nullptr, static_cast<uint16_t>(index), v);
  }
  bool u64(uint64_t v) const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_unsigned_int
                     ->set(nullptr, v)
               : mysql_service_mysql_stored_program_runtime_argument_unsigned_int
                     ->set(nullptr, static_cast<uint16_t>(index), v);
  }
  bool f64(double v) const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_float->set(
                     nullptr, v)
               : mysql_service_mysql_stored_program_runtime_argument_float->set(
                     nullptr, static_cast<uint16_t>(index), v);
  }
  /** utf8mb4 text */
  bool text(const std::string &s) const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_string->set(
                     nullptr, s.data(), s.size())
               : mysql_service_mysql_stored_program_runtime_argument_string
                     ->set(nullptr, static_cast<uint16_t>(index), s.data(),
                           s.size());
  }
  bool binary(const char *data, size_t n) const {
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_string_charset
                     ->set(nullptr, data, n, binary_charset())
               : mysql_service_mysql_stored_program_runtime_argument_string_charset
                     ->set(nullptr, static_cast<uint16_t>(index), data, n,
                           binary_charset());
  }
};

bool store(const Target &to, const Value &v) {
  switch (v.tag()) {
    case Tag::kNull:
      return to.null();
    case Tag::kI64:
      return to.i64(v.as_i64());
    case Tag::kU64:
      return to.u64(v.as_u64());
    case Tag::kF64:
      return to.f64(v.as_f64());
    case Tag::kF32:
      return to.f64(v.as_f32());
    case Tag::kDecimal:
    case Tag::kString:
    case Tag::kJson:
      return to.text(v.as_text());
    case Tag::kBytes:
      return to.binary(v.as_text().data(), v.as_text().size());
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
    case Tag::kTimestamp:
      return to.text(format_temporal(v.tag(), v.as_temporal()));
    case Tag::kVector: {
      const auto &f = v.as_vector();
      return to.binary(reinterpret_cast<const char *>(f.data()),
                       f.size() * sizeof(float));
    }
  }
  return true;
}

void report_read_failure(const ParamInfo &p) {
  my_error(ER_LANGUAGE_COMPONENT, MYF(0),
           ("MLE-Type> Cannot read the value of parameter '" + p.name + "'")
               .c_str());
}

}  // namespace

long routines_alive() { return g_alive.load(); }

const char *display_language(const std::string &l) {
  if (l == "js") return "JavaScript";
  if (l == "python") return "Python";
  if (l == "java") return "Java";
  return l.c_str();
}

const char *Routine::display_language() const {
  return language == kJavaScript ? "JavaScript"
         : language == kPython   ? "Python"
                                 : "Java";
}

Routine *Routine::create(stored_program_handle sp) {
  auto get_string = [&](const char *key) {
    mysql_cstring_with_length v{nullptr, 0};
    if (mysql_service_mysql_stored_program_metadata_query->get(sp, key, &v) ||
        v.str == nullptr)
      return std::string();
    return std::string(v.str, v.length);
  };
  const std::string language = get_string("sp_language");
  const unsigned bit = language_bit(language);
  if ((bit & enabled_languages()) == 0) {
    my_error(ER_LANGUAGE_COMPONENT_UNSUPPORTED_LANGUAGE, MYF(0),
             language.c_str());
    return nullptr;
  }
  uint16_t type = 0;
  uint32_t n = 0;
  if (mysql_service_mysql_stored_program_metadata_query->get(sp, "sp_type",
                                                             &type) ||
      mysql_service_mysql_stored_program_metadata_query->get(
          sp, "argument_count", &n))
    return nullptr;
  if (type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION &&
      type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> Only stored functions and procedures are supported.");
    return nullptr;
  }

  auto r = std::make_unique<Routine>();
  r->language = bit;
  r->sp_type = type;
  r->name = get_string("sp_name");
  r->qualified_name = get_string("qualified_name");
  r->schema = get_string("database_name");
  for (uint32_t i = 0; i < n; i++) {
    ParamInfo p;
    const char *name = nullptr;
    bool in = true, out = false;
    const TypeQuery q{sp, static_cast<int>(i)};
    (void)q.get("argument_name", &name);
    (void)q.get("in_variable", &in);
    (void)q.get("out_variable", &out);
    p.name = name == nullptr ? "" : name;
    p.mode = out ? (in ? ParamMode::kInOut : ParamMode::kOut) : ParamMode::kIn;
    if (!valid_name(p.name, bit)) {
      my_error(ER_LANGUAGE_COMPONENT_KEYWORD, MYF(0), r->display_language(),
               p.name.c_str());
      return nullptr;
    }
    if (!describe(q, "parameter '" + p.name + "'", &p)) return nullptr;
    r->params.push_back(std::move(p));
  }
  if (r->is_function()) {
    ParamInfo ret;
    ret.name = "return value";
    if (!describe(TypeQuery{sp, -1}, "return value", &ret)) return nullptr;
    r->ret = std::move(ret);
  }

  r->id = ++g_next_id;
  mle::PrepareMsg &m = r->prepare;
  m.routine_id = r->id;
  m.language = worker_language(bit);
  m.name = r->name;
  m.source = get_string("sp_body");
  // USING (Phase 6): the libraries the routine imports.
  uint32_t imports = 0;
  (void)mysql_service_mysql_stored_program_metadata_query->get(sp, "import_count", &imports);
  for (uint32_t i = 0; i < imports; i++) {
    mysql_cstring_with_length schema{nullptr, 0}, lib{nullptr, 0}, version{nullptr, 0},
        alias{nullptr, 0};
    if (mysql_service_mysql_stored_program_import_metadata_query->get(
            sp, i, &schema, &lib, &version, &alias, nullptr))
      continue;
    mle::Import imp;
    imp.schema.assign(schema.str == nullptr ? r->schema.c_str() : schema.str,
                      schema.str == nullptr ? r->schema.size() : schema.length);
    if (lib.str != nullptr) imp.name.assign(lib.str, lib.length);
    if (alias.str != nullptr) imp.alias.assign(alias.str, alias.length);
    m.imports.push_back(std::move(imp));
  }
  for (const ParamInfo &p : r->params) m.params.push_back({p.name, p.mode, p.type});
  if (r->ret) m.returns = r->ret->type;
  ++g_alive;
  return r.release();
}

bool Routine::read_args(std::vector<Value> *args) const {
  args->clear();
  args->reserve(params.size());
  for (uint16_t i = 0; i < params.size(); i++) {
    const ParamInfo &p = params[i];
    if (p.mode == ParamMode::kOut) {
      args->push_back(Value::null());
      continue;
    }
    bool is_null = false;
    switch (p.kind) {
      case Kind::kInt: {
        int64_t v = 0;
        if (mysql_service_mysql_stored_program_runtime_argument_int->get(
                nullptr, i, &v, &is_null))
          return report_read_failure(p), false;
        args->push_back(is_null ? Value::null() : Value::i64(v));
        break;
      }
      case Kind::kUInt:
      case Kind::kBigUInt: {
        uint64_t v = 0;
        if (mysql_service_mysql_stored_program_runtime_argument_unsigned_int
                ->get(nullptr, i, &v, &is_null))
          return report_read_failure(p), false;
        args->push_back(is_null                   ? Value::null()
                        : p.kind == Kind::kUInt ? Value::i64(int64_t(v))
                                                  : Value::u64(v));
        break;
      }
      case Kind::kFloat:
      case Kind::kDouble: {
        double v = 0;
        if (mysql_service_mysql_stored_program_runtime_argument_float->get(
                nullptr, i, &v, &is_null))
          return report_read_failure(p), false;
        args->push_back(is_null                 ? Value::null()
                        : p.kind == Kind::kFloat ? Value::f32(float(v))
                                                 : Value::f64(v));
        break;
      }
      default: {
        const char *s = nullptr;
        size_t n = 0;
        if (mysql_service_mysql_stored_program_runtime_argument_string->get(
                nullptr, i, &s, &n, &is_null))
          return report_read_failure(p), false;
        if (is_null) {
          args->push_back(Value::null());
          break;
        }
        switch (p.kind) {
          case Kind::kDecimal:
            args->push_back(Value::decimal(std::string(s, n)));
            break;
          case Kind::kString:
            args->push_back(Value::string(std::string(s, n)));
            break;
          case Kind::kBytes:
            args->push_back(Value::bytes(std::string(s, n)));
            break;
          case Kind::kJson:
            args->push_back(Value::json(std::string(s, n)));
            break;
          case Kind::kVector: {
            std::vector<float> f(n / sizeof(float));
            memcpy(f.data(), s, f.size() * sizeof(float));
            args->push_back(Value::vector(std::move(f)));
            break;
          }
          default: {
            mle::Temporal t;
            if (!parse_temporal(p.kind, s, n, &t))
              return report_read_failure(p), false;
            args->push_back(Value::temporal(p.type.tag, t));
          }
        }
      }
    }
  }
  return true;
}

bool Routine::store_results(const mle::ResultMsg &result) const {
  if (is_function()) {
    if (!result.ret) return true;
    if (store(Target{-1}, *result.ret)) return false;
  }
  for (const auto &[index, value] : result.out_params)
    if (store(Target{static_cast<int>(index)}, value)) return false;
  return true;
}

std::string Routine::call_text(const std::vector<Value> &args) const {
  std::string s = name + "(";
  for (size_t i = 0; i < args.size(); i++) {
    if (i > 0) s += ", ";
    const Value &v = args[i];
    switch (v.tag()) {
      case Tag::kNull:
        s += "NULL";
        break;
      case Tag::kI64:
        s += std::to_string(v.as_i64());
        break;
      case Tag::kU64:
        s += std::to_string(v.as_u64());
        break;
      case Tag::kF64:
      case Tag::kF32: {
        char b[32];
        snprintf(b, sizeof b, "%g",
                 v.tag() == Tag::kF64 ? v.as_f64() : double(v.as_f32()));
        s += b;
        break;
      }
      case Tag::kDecimal:
        s += v.as_text();
        break;
      case Tag::kString:
      case Tag::kJson:
        s += "'" + v.as_text() + "'";
        break;
      case Tag::kDate:
      case Tag::kTime:
      case Tag::kDatetime:
      case Tag::kTimestamp:
        s += "'" + format_temporal(v.tag(), v.as_temporal()) + "'";
        break;
      default:
        s += "...";
    }
  }
  return s + ")";
}

void Routine::destroy(Routine *r) {
  if (r == nullptr) return;
  --g_alive;
  delete r;
}

}  // namespace mle_component
