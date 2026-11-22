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
#include <cerrno>
#include <cstring>
#include <ctime>
#include <set>
#include <stdexcept>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysql/components/services/mysql_string.h>
#include <mysql/components/services/mysql_thd_attributes.h>
#include <mysql/mysql_lex_string.h>
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
extern REQUIRES_SERVICE_PLACEHOLDER(
    mysql_stored_program_runtime_argument_timestamp);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_timestamp);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_attributes);
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

const char *worker_language(unsigned bit) {
  // The language the worker runs: JS (the V8 worker) is JavaScript, CPYTHON
  // (the CPython worker) is Python (exp design note 63).
  switch (family(bit)) {
    case kJavaScript:
      return "js";
    case kPython:
      return "python";
    case kJava:
      return "java";
    default:
      return "";
  }
}

// ------------------------------------------------------------- time zones

std::string_view session_time_zone_name(MYSQL_THD thd) {
  MYSQL_LEX_CSTRING name{nullptr, 0};
  if (mysql_service_mysql_thd_attributes->get(thd, "time_zone_name", &name) ||
      name.str == nullptr)
    return {};
  return {name.str, name.length};
}

std::string session_time_zone(MYSQL_THD thd) {
  return std::string(session_time_zone_name(thd));
}

namespace {

/** "+05:30", "-3:00" -> seconds east of UTC. */
bool parse_offset(std::string_view text, int32_t *seconds) {
  if (text.size() < 5 || text.size() > 6 || (text[0] != '+' && text[0] != '-'))
    return false;
  const size_t colon = text.find(':');
  if (colon != text.size() - 3) return false;
  int hours = 0, minutes = 0;
  for (size_t i = 1; i < text.size(); i++) {
    if (i == colon) continue;
    if (text[i] < '0' || text[i] > '9') return false;
    (i < colon ? hours : minutes) = (i < colon ? hours : minutes) * 10 + (text[i] - '0');
  }
  if (minutes > 59) return false;
  const int32_t total = hours * 3600 + minutes * 60;
  // MySQL's and the protocol's range: -13:59 to +14:00.
  if (text[0] == '-' ? total > 13 * 3600 + 59 * 60 : total > 14 * 3600)
    return false;
  *seconds = text[0] == '-' ? -total : total;
  return true;
}

std::string offset_name(int32_t seconds) {
  char b[16];
  const int32_t a = seconds < 0 ? -seconds : seconds;
  snprintf(b, sizeof b, "%c%02d:%02d", seconds < 0 ? '-' : '+', a / 3600,
           a % 3600 / 60);
  return b;
}

}  // namespace

namespace {

std::atomic<uint64_t> g_zone_ids{0};

/** Around a change of offset local times repeat or are skipped: the
    interval a TimeZone remembers ends this far from each change (no zone
    moves its clock by more). */
constexpr int64_t kChangeMargin = 26 * 3600;

}  // namespace

bool TimeZone::set(std::string_view name) {
  m_id = ++g_zone_ids;
  m_zone = nullptr;
  m_from = m_to = 0;
  m_offset = 0;
  if (int32_t offset = 0; parse_offset(name, &offset)) {
    // One offset for all time.
    m_from = INT64_MIN;
    m_to = INT64_MAX;
    m_offset = offset;
    return true;
  }
  // SYSTEM is mysqld's own zone: by its name in the zone data if it has
  // one, else by the C library (m_zone stays nullptr).
  const std::string system = name == "SYSTEM" ? system_time_zone() : "";
  if (name == "SYSTEM") {
    int32_t offset = 0;
    if (system.empty() || parse_offset(system, &offset)) return true;
    name = system;
  }
  try {
    m_zone = std::chrono::locate_zone(name);
  } catch (const std::runtime_error &) {
    // No such zone, or no zone data on this host: for SYSTEM the C library
    // still converts.
    return !system.empty();
  }
  return true;
}

bool TimeZone::lookup(int64_t local, const mle::Temporal &t, int64_t *seconds) {
  namespace ch = std::chrono;
  if (m_zone == nullptr) {
    // mysqld's own zone, by the C library's rules.
    struct tm tm {};
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1;
    tm.tm_mday = t.day;
    tm.tm_hour = static_cast<int>(t.hour);
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
    tm.tm_isdst = -1;
    errno = 0;
    const time_t at = mktime(&tm);
    if (at == static_cast<time_t>(-1) && errno != 0) return false;
    *seconds = at;
    return true;
  }
  const ch::local_info info = m_zone->get_info(ch::local_seconds{ch::seconds{local}});
  switch (info.result) {
    case ch::local_info::unique: {
      const int64_t offset = info.first.offset.count();
      *seconds = local - offset;
      // The offset holds from first.begin to first.end; near either end a
      // local time may belong to the neighbour too.
      const int64_t begin = info.first.begin.time_since_epoch().count();
      const int64_t end = info.first.end.time_since_epoch().count();
      constexpr int64_t kFar = INT64_MAX / 4;
      m_from = begin < -kFar ? INT64_MIN : begin + offset + kChangeMargin;
      m_to = end > kFar ? INT64_MAX : end + offset - kChangeMargin;
      m_offset = offset;
      return true;
    }
    case ch::local_info::ambiguous:
      // The zone repeats this local time: its first occurrence (as the
      // server takes it).
      *seconds = local - info.first.offset.count();
      return true;
    case ch::local_info::nonexistent:
      // The zone skips this local time: the instant of the change (as the
      // server takes it).
      *seconds = info.first.end.time_since_epoch().count();
      return true;
  }
  return false;
}

bool TimeZone::instant(const mle::Temporal &t, int64_t *microseconds) {
  if (t.year == 0 && t.month == 0 && t.day == 0 && t.hour == 0 &&
      t.minute == 0 && t.second == 0 && t.microsecond == 0) {
    *microseconds = mle::wire::kZeroTimestamp;
    return true;
  }
  namespace ch = std::chrono;
  const ch::year_month_day date{ch::year{t.year}, ch::month{t.month}, ch::day{t.day}};
  if (!date.ok() || t.hour > 23 || t.minute > 59 || t.second > 59 ||
      t.microsecond > 999999)
    return false;
  const int64_t local = ch::local_days{date}.time_since_epoch().count() * int64_t{86400} +
                        t.hour * 3600 + t.minute * 60 + t.second;
  int64_t seconds = local - m_offset;
  if ((local < m_from || local >= m_to) && !lookup(local, t, &seconds))
    return false;
  *microseconds = seconds * 1000000 + t.microsecond;
  return mle::is_valid_timestamp(*microseconds) &&
         *microseconds != mle::wire::kZeroTimestamp;
}

TimeZone *ZoneCache::current(MYSQL_THD thd, std::string *name) {
  const std::string_view now = session_time_zone_name(thd);
  if (!m_known || now != m_name) {
    m_name.assign(now);
    m_known = m_zone.set(now);
  }
  if (m_known) return &m_zone;
  if (name != nullptr) *name = m_name;
  return nullptr;
}

std::string options_time_zone(std::string_view zone) {
  if (zone == "SYSTEM") return system_time_zone();
  if (int32_t offset = 0; parse_offset(zone, &offset)) return offset_name(offset);
  // Zones that count leap seconds have no instants the protocol can carry.
  if (zone.empty() || zone.size() > mle::wire::kMaxTimeZone ||
      zone.rfind("leap/", 0) == 0 || zone.rfind("right/", 0) == 0 ||
      !mle::is_valid_utf8(zone))
    return "";
  return std::string(zone);
}

mle::Temporal utc_fields(int64_t microseconds) {
  namespace ch = std::chrono;
  const ch::sys_time<ch::microseconds> at{ch::microseconds{microseconds}};
  const auto day = ch::floor<ch::days>(at);
  const ch::year_month_day date{day};
  const ch::hh_mm_ss<ch::microseconds> time{at - day};
  mle::Temporal t;
  t.year = static_cast<uint16_t>(static_cast<int>(date.year()));
  t.month = static_cast<uint8_t>(static_cast<unsigned>(date.month()));
  t.day = static_cast<uint8_t>(static_cast<unsigned>(date.day()));
  t.hour = static_cast<uint32_t>(time.hours().count());
  t.minute = static_cast<uint8_t>(time.minutes().count());
  t.second = static_cast<uint8_t>(time.seconds().count());
  t.microsecond = static_cast<uint32_t>(time.subseconds().count());
  return t;
}

uint32_t decimal_precision(uint64_t display_length, uint32_t scale, bool is_unsigned) {
  // The display length counts the digits, the point if there is a scale and
  // the sign if the type is signed.
  uint64_t n = display_length;
  if (scale > 0 && n > 0) n--;
  if (!is_unsigned && n > 0) n--;
  return static_cast<uint32_t>(n);
}

namespace {

std::atomic<uint64_t> g_next_id{0};
std::atomic<long> g_alive{0};

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
  const unsigned f = family(language);
  return f == kJavaScript ? js : f == kPython ? python : java;
}

/** A parameter name the language accepts as a variable. */
bool valid_name(const std::string &name, unsigned bit) {
  const unsigned language = family(bit);
  if (name.empty() || reserved(language).count(name) != 0) return false;
  if (language == kJavaScript && name.size() > 2 && name[0] == '$' &&
      name[1] == 'a' && isdigit(static_cast<unsigned char>(name[2])))
    return false;  // $a0, $a1, ...: the wrapper's argument names
  if (language == kJavaScript && (name == "$c" || name == "$G" || name == "$r"))
    return false;  // the wrapper's own names
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
    // Routine parameters have the NEW/2 codes, result columns the others.
    case MYSQL_SP_ARG_TYPE_DECIMAL:
    case MYSQL_SP_ARG_TYPE_NEWDECIMAL:
      return "DECIMAL";
    case MYSQL_SP_ARG_TYPE_DATE:
    case MYSQL_SP_ARG_TYPE_NEWDATE:
      return "DATE";
    case MYSQL_SP_ARG_TYPE_TIME:
    case MYSQL_SP_ARG_TYPE_TIME2:
      return "TIME";
    case MYSQL_SP_ARG_TYPE_DATETIME:
    case MYSQL_SP_ARG_TYPE_DATETIME2:
      return "DATETIME";
    case MYSQL_SP_ARG_TYPE_TIMESTAMP:
    case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
      return "TIMESTAMP";
    case MYSQL_SP_ARG_TYPE_JSON:
      return "JSON";
    case MYSQL_SP_ARG_TYPE_VARCHAR:
    case MYSQL_SP_ARG_TYPE_VAR_STRING:
      return "VARCHAR";
    case MYSQL_SP_ARG_TYPE_NULL:
      return "NULL";
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

/** The fractional-second digits of a TIME, DATETIME or TIMESTAMP. */
uint32_t fraction_digits(const TypeQuery &q) {
  uint32_t decimals = 0;
  (void)q.get("decimals", &decimals);
  return decimals <= 6 ? decimals : 0;
}

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
    case MYSQL_SP_ARG_TYPE_NEWDECIMAL: {
      uint32_t decimals = 0;
      size_t display = 0;
      (void)q.get("decimals", &decimals);
      (void)q.get("max_display_length", &display);
      p.scale = decimals;
      p.precision = decimal_precision(display, decimals, !is_signed);
      p.tag = Tag::kDecimal;
      info->kind = Kind::kDecimal;
      return true;
    }
    case MYSQL_SP_ARG_TYPE_NEWDATE:
      p.tag = Tag::kDate;
      info->kind = Kind::kDate;
      return true;
    case MYSQL_SP_ARG_TYPE_TIME2:
      p.tag = Tag::kTime;
      p.scale = fraction_digits(q);
      info->kind = Kind::kTime;
      return true;
    case MYSQL_SP_ARG_TYPE_DATETIME2:
      p.tag = Tag::kDatetime;
      p.scale = fraction_digits(q);
      info->kind = Kind::kDatetime;
      return true;
    case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
      p.tag = Tag::kTimestamp;
      p.scale = fraction_digits(q);
      info->kind = Kind::kTimestamp;
      return true;
    case MYSQL_SP_ARG_TYPE_JSON:
      p.tag = Tag::kJson;
      info->kind = Kind::kJson;
      return true;
    case MYSQL_SP_ARG_TYPE_VECTOR: {
      size_t bytes = 0;
      (void)q.get("max_byte_length", &bytes);
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
      size_t chars = 0;
      (void)q.get("max_display_length", &chars);
      p.length = static_cast<uint32_t>(chars);
      if (cs == "binary" && t != MYSQL_SP_ARG_TYPE_ENUM &&
          t != MYSQL_SP_ARG_TYPE_SET) {
        p.tag = Tag::kBytes;
        info->kind = Kind::kBytes;
        if (p.type_name == "VARCHAR") p.type_name = "VARBINARY";
        if (p.type_name == "CHAR") p.type_name = "BINARY";
        return true;
      }
      // A string parameter or return type is declared utf8mb4 or binary
      // (protocol Appendix A, Oracle's rule): the server converts values of
      // other character sets when it passes and stores them. Binary ENUM
      // and SET members go as they are.
      if (cs != "utf8mb4" && cs != "binary")
        return unsupported("The character set " + cs +
                           " is not supported; use utf8mb4 or binary");
      p.tag = Tag::kString;
      info->kind = Kind::kString;
      if (p.type_name == "BLOB") p.type_name = "TEXT";
      return true;
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

/** An instant as text with its offset, which the server reads in any
    session time zone: "2024-01-02 03:04:05.000000+00:00". */
std::string format_instant(int64_t microseconds) {
  if (microseconds == mle::wire::kZeroTimestamp)
    return "0000-00-00 00:00:00.000000";
  const mle::Temporal t = utc_fields(microseconds);
  char b[64];
  snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u:%02u.%06u+00:00", t.year,
           t.month, t.day, t.hour, t.minute, t.second, t.microsecond);
  return b;
}

/** A DATE, TIME or DATETIME as the server's text. */
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
  /** An instant (not the zero timestamp): the server converts it to the
      session's zone, or to what the target's type needs. */
  bool instant(int64_t microseconds) const {
    const mle::Temporal t = utc_fields(microseconds);
    constexpr uint32_t kDigits = 6;
    return index < 0
               ? mysql_service_mysql_stored_program_return_value_timestamp->set(
                     nullptr, t.year, t.month, t.day, t.hour, t.minute,
                     t.second, t.microsecond, false, kDigits, 0, true)
               : mysql_service_mysql_stored_program_runtime_argument_timestamp
                     ->set(nullptr, static_cast<uint16_t>(index), t.year,
                           t.month, t.day, t.hour, t.minute, t.second,
                           t.microsecond, false, kDigits, 0, true);
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
      return to.text(format_temporal(v.tag(), v.as_temporal()));
    case Tag::kTimestamp:
      // The zero timestamp is no instant: as the server writes it.
      if (v.as_timestamp() == mle::wire::kZeroTimestamp)
        return to.text(format_instant(v.as_timestamp()));
      return to.instant(v.as_timestamp());
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
  const unsigned f = family(language);
  return f == kJavaScript ? "JavaScript" : f == kPython ? "Python" : "Java";
}

uint64_t next_routine_id() { return ++g_next_id; }

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
  // Known once the host is: an anonymous definer has an empty user name.
  if (const std::string host = get_string("sp_definer_host"); !host.empty())
    r->definer_key = account_key(get_string("sp_definer_user"), host);
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
  m.schema = r->schema;
  m.source = get_string("sp_body");
  // Source code crosses as it is or not at all: text that is not UTF-8
  // would become other code if it were repaired (protocol §7.1). Names are
  // for messages: repaired.
  if (!mle::is_valid_utf8(m.source)) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             ("MLE> The code of " + r->qualified_name +
              " is not valid UTF-8 text; create the routine over a connection "
              "whose character set is utf8mb4")
                 .c_str());
    return nullptr;
  }
  repair_utf8(&m.name);
  if (m.schema.empty() || !mle::is_valid_utf8(m.schema)) {
    my_error(ER_LANGUAGE_COMPONENT, MYF(0),
             "MLE> The routine has no schema name MLE can use.");
    return nullptr;
  }
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
    // The name the routine's code uses for the library is put into that
    // code (JavaScript: the wrapper's text): an identifier like a
    // parameter's, and not a parameter's or another library's.
    const std::string &binding = imp.alias.empty() ? imp.name : imp.alias;
    // Java code names a library by its own means: there only the names of
    // two libraries must differ, as PREPARE requires of every language.
    bool taken = false, twice = false;
    for (const ParamInfo &p : r->params) taken = taken || p.name == binding;
    for (const mle::Import &other : m.imports)
      twice = twice || (other.alias.empty() ? other.name : other.alias) == binding;
    if (twice || (family(bit) != kJava && (taken || !valid_name(binding, bit)))) {
      my_error(ER_LANGUAGE_COMPONENT_KEYWORD, MYF(0), r->display_language(),
               binding.c_str());
      return nullptr;
    }
    // A library's schema and name cross as they are (never repaired).
    if (!mle::is_valid_library_name(imp.schema) ||
        !mle::is_valid_library_name(imp.name) || !mle::is_valid_utf8(imp.schema) ||
        !mle::is_valid_utf8(imp.name) || !mle::is_valid_utf8(imp.alias)) {
      my_error(ER_LANGUAGE_COMPONENT, MYF(0),
               ("MLE> The routine imports a library whose name MLE cannot use: " +
                imp.schema + "." + imp.name)
                   .c_str());
      return nullptr;
    }
    m.imports.push_back(std::move(imp));
  }
  for (const ParamInfo &p : r->params) m.params.push_back({p.name, p.mode, p.type});
  if (r->ret) m.returns = r->ret->type;
  ++g_alive;
  return r.release();
}

bool Routine::read_args(std::vector<Value> *args, ZoneCache *zones) const {
  args->clear();
  args->reserve(params.size());
  // The session's zone, asked for at the first TIMESTAMP argument.
  TimeZone *zone = nullptr;
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
          case Kind::kTimestamp: {
            // The server gives the local time in the session's zone.
            mle::Temporal local;
            if (!parse_temporal(p.kind, s, n, &local))
              return report_read_failure(p), false;
            if (zone == nullptr) {
              std::string name;
              zone = zones->current(nullptr, &name);
              if (zone == nullptr) {
                my_error(ER_LANGUAGE_COMPONENT, MYF(0),
                         ("MLE-Type> Cannot convert the TIMESTAMP parameter '" +
                          p.name + "': the session's time zone '" + name +
                          "' is not known to MLE")
                             .c_str());
                return false;
              }
            }
            int64_t at = 0;
            if (!zone->instant(local, &at)) return report_read_failure(p), false;
            args->push_back(Value::timestamp(at));
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
        s += "'" + format_temporal(v.tag(), v.as_temporal()) + "'";
        break;
      case Tag::kTimestamp:
        s += "'" + format_instant(v.as_timestamp()) + "'";
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
