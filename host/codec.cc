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

#include "mle/codec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace mle {

static_assert(std::endian::native == std::endian::little,
              "the MLE wire protocol is little-endian; add byte swapping");

namespace {

// Smallest encodings, used to bound element counts.
constexpr size_t kMinValue = 1;                      // tag only (NULL)
constexpr size_t kMinStr = 4;                        // empty string
constexpr size_t kMinParamType = 1 + 1 + 4 + 4 + 4 + kMinStr;
constexpr size_t kMinParam = kMinStr + 1 + kMinParamType;
constexpr size_t kMinColumn = 7 * kMinStr + kMinParamType + 4;
constexpr size_t kMinFrame = 3 * kMinStr + 4 + 4 + 4;
constexpr size_t kMinWarning = 4 + 4 + kMinStr;
constexpr size_t kMinOutParam = 4 + kMinValue;
constexpr size_t kTemporalSize = 2 + 1 + 1 + 4 + 1 + 1 + 4 + 1;

size_t vector_padding(size_t offset) {
  return (wire::kVectorAlignment - offset % wire::kVectorAlignment) %
         wire::kVectorAlignment;
}

void put_param_type(Writer &w, const ParamType &t) {
  w.u8(static_cast<uint8_t>(t.tag));
  w.flag(t.is_unsigned);
  w.u32(t.length);
  w.u32(t.precision);
  w.u32(t.scale);
  w.str(t.type_name);
}

bool get_tag(Reader &r, Tag *tag) {
  uint8_t t;
  if (!r.u8(&t)) return false;
  if (t > wire::kMaxTag)
    return r.fail("unknown value tag " + std::to_string(t));
  *tag = static_cast<Tag>(t);
  return true;
}

bool get_param_type(Reader &r, ParamType *t) {
  return get_tag(r, &t->tag) && r.flag(&t->is_unsigned) && r.u32(&t->length) &&
         r.u32(&t->precision) && r.u32(&t->scale) && r.str(&t->type_name);
}

void put_values(Writer &w, const std::vector<Value> &values) {
  w.u32(static_cast<uint32_t>(values.size()));
  for (const Value &v : values) w.value(v);
}

bool get_values(Reader &r, std::vector<Value> *values) {
  uint32_t n;
  if (!r.count(&n, kMinValue, wire::kMaxValues)) return false;
  values->clear();
  values->resize(n);
  for (Value &v : *values)
    if (!r.value(&v)) return false;
  return true;
}

void put_warnings(Writer &w, const std::vector<Warning> &warnings) {
  w.u32(static_cast<uint32_t>(warnings.size()));
  for (const Warning &x : warnings) {
    w.u32(x.level);
    w.u32(x.code);
    w.str(x.message);
  }
}

bool get_warnings(Reader &r, std::vector<Warning> *warnings) {
  uint32_t n;
  if (!r.count(&n, kMinWarning, wire::kMaxWarnings)) return false;
  warnings->clear();
  warnings->resize(n);
  for (Warning &x : *warnings) {
    if (!r.u32(&x.level) || !r.u32(&x.code) || !r.str(&x.message)) return false;
    if (x.level > static_cast<uint32_t>(wire::WarningLevel::kError))
      return r.fail("bad warning level");
  }
  return true;
}

void put_error(Writer &w, const Error &e) {
  w.str(e.language);
  w.str(e.message);
  w.u32(static_cast<uint32_t>(e.stack.size()));
  for (const StackFrame &f : e.stack) {
    w.str(f.function);
    w.str(f.file);
    w.u32(f.line);
    w.u32(f.column);
    w.u32(f.end_column);
    w.str(f.text);
  }
  w.u32(e.sql_errno);
  w.str(e.sqlstate);
  w.u64(e.sql_error_id);
}

/** An Error of a worker, with the caps of §7.4. Whether its language is
    one the worker may name is the receiver's check: it knows the READY. */
bool get_error(Reader &r, Error *e) {
  uint32_t n;
  if (!r.str(&e->language) || !r.str(&e->message, wire::kMaxErrorMessage) ||
      !r.count(&n, kMinFrame, wire::kMaxStackFrames))
    return false;
  e->stack.clear();
  e->stack.resize(n);
  for (StackFrame &f : e->stack)
    if (!r.str(&f.function, wire::kMaxFrameText) ||
        !r.str(&f.file, wire::kMaxFrameText) || !r.u32(&f.line) ||
        !r.u32(&f.column) || !r.u32(&f.end_column) ||
        !r.str(&f.text, wire::kMaxFrameText))
      return false;
  if (!r.u32(&e->sql_errno) || !r.str(&e->sqlstate) ||
      !r.u64(&e->sql_error_id))
    return false;
  if (e->sql_errno > wire::kMaxSqlErrno) return r.fail("bad SQL error number");
  if (!is_valid_sqlstate(e->sqlstate)) return r.fail("bad SQLSTATE");
  return true;
}

template <class M, class F>
bool decode_with(std::string_view body, Sender from, M *m, std::string *error,
                 F &&fn) {
  Reader r(body, from);
  *m = M();
  if (fn(r, m) && r.finish()) return true;
  if (error != nullptr) *error = r.error();
  return false;
}

bool options_problem(const OptionsMsg &m, const char **why) {
  if (m.time_zone.empty() || m.time_zone.size() > wire::kMaxTimeZone)
    return *why = "bad time zone", true;
  if (m.settings.size() > wire::kMaxSettings)
    return *why = "too many settings", true;
  const std::string *previous = nullptr;
  for (const Setting &s : m.settings) {
    if (s.name.empty() || s.name.size() > wire::kMaxSettingName ||
        !std::all_of(s.name.begin(), s.name.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                 c == '.';
        }))
      return *why = "bad setting name", true;
    if (s.value.size() > wire::kMaxSettingValue)
      return *why = "setting value too long", true;
    if (previous != nullptr && !(*previous < s.name))
      return *why = "settings are not sorted", true;
    previous = &s.name;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- Value

bool Value::operator==(const Value &other) const {
  if (m_tag != other.m_tag) return false;
  switch (m_tag) {
    case Tag::kF64:
      return std::bit_cast<uint64_t>(as_f64()) ==
             std::bit_cast<uint64_t>(other.as_f64());
    case Tag::kF32:
      return std::bit_cast<uint32_t>(as_f32()) ==
             std::bit_cast<uint32_t>(other.as_f32());
    case Tag::kVector: {
      const auto &a = as_vector();
      const auto &b = other.as_vector();
      return a.size() == b.size() &&
             (a.empty() ||
              std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }
    default:
      return m_data == other.m_data;
  }
}

const char *tag_name(Tag tag) {
  switch (tag) {
    case Tag::kNull:
      return "NULL";
    case Tag::kI64:
      return "I64";
    case Tag::kU64:
      return "U64";
    case Tag::kF64:
      return "F64";
    case Tag::kF32:
      return "F32";
    case Tag::kDecimal:
      return "DECIMAL";
    case Tag::kString:
      return "STRING";
    case Tag::kBytes:
      return "BYTES";
    case Tag::kDate:
      return "DATE";
    case Tag::kTime:
      return "TIME";
    case Tag::kDatetime:
      return "DATETIME";
    case Tag::kTimestamp:
      return "TIMESTAMP";
    case Tag::kJson:
      return "JSON";
    case Tag::kVector:
      return "VECTOR";
  }
  return "?";
}

bool is_finite(const Value &v) {
  switch (v.tag()) {
    case Tag::kF64:
      return std::isfinite(v.as_f64());
    case Tag::kF32:
      return std::isfinite(v.as_f32());
    case Tag::kVector: {
      const auto &f = v.as_vector();
      return std::all_of(f.begin(), f.end(),
                         [](float x) { return std::isfinite(x); });
    }
    default:
      return true;
  }
}

bool all_finite(std::span<const Value> values) {
  return std::all_of(values.begin(), values.end(),
                     [](const Value &v) { return is_finite(v); });
}

// ---------------------------------------------------------------- checks

bool is_valid_utf8(std::string_view s) {
  const auto *p = reinterpret_cast<const unsigned char *>(s.data());
  const auto *end = p + s.size();
  while (p < end) {
    unsigned char c = *p;
    if (c < 0x80) {
      p++;
      continue;
    }
    size_t n;
    uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) {
      n = 2, cp = c & 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      n = 3, cp = c & 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      n = 4, cp = c & 0x07, min = 0x10000;
    } else {
      return false;
    }
    if (static_cast<size_t>(end - p) < n) return false;
    for (size_t i = 1; i < n; i++) {
      if ((p[i] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    p += n;
  }
  return true;
}

bool is_valid_decimal(std::string_view s) {
  if (s.empty() || s.size() > wire::kMaxDecimalChars) return false;
  size_t i = 0;
  if (s[0] == '-') i++;
  size_t int_digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, int_digits++;
  if (int_digits == 0) return false;
  if (i == s.size()) return true;
  if (s[i++] != '.') return false;
  size_t frac_digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, frac_digits++;
  return frac_digits > 0 && i == s.size();
}

bool is_valid_temporal(Tag tag, const Temporal &t) {
  const bool time_ok =
      t.minute <= 59 && t.second <= 59 && t.microsecond <= 999999;
  switch (tag) {
    case Tag::kDate:
      return t.year <= 9999 && t.month <= 12 && t.day <= 31 && t.hour == 0 &&
             t.minute == 0 && t.second == 0 && t.microsecond == 0 &&
             !t.negative;
    case Tag::kDatetime:
      return t.year <= 9999 && t.month <= 12 && t.day <= 31 && t.hour <= 23 &&
             time_ok && !t.negative;
    case Tag::kTime: {
      if (t.year != 0 || t.month != 0 || t.day != 0 || !time_ok) return false;
      if (t.hour < wire::kMaxTimeHour) return true;
      // 838:59:59.000000 is the largest TIME: all of hour 838 up to it.
      return t.hour == wire::kMaxTimeHour &&
             !(t.minute == 59 && t.second == 59 && t.microsecond != 0);
    }
    default:
      return false;
  }
}

bool is_valid_timestamp(int64_t microseconds) {
  return microseconds == wire::kZeroTimestamp ||
         (microseconds >= 0 && microseconds <= wire::kMaxTimestamp);
}

bool is_valid_identifier(std::string_view s) {
  return !s.empty() && s.size() <= wire::kMaxIdentifier &&
         std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

bool is_valid_sqlstate(std::string_view s) {
  if (s.empty()) return true;
  return s.size() == 5 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
         });
}

bool is_valid_library_name(std::string_view s) {
  return !s.empty() && s.size() <= wire::kMaxLibraryName &&
         s.find('\0') == std::string_view::npos;
}

bool is_valid(const OptionsMsg &m, std::string *error) {
  const char *why = nullptr;
  if (!options_problem(m, &why)) return true;
  if (error != nullptr) *error = why;
  return false;
}

// ---------------------------------------------------------------- Writer

namespace {

/** An upper bound of the encoded size of values (Writer::value). */
size_t encoded_size(const std::vector<Value> &values) {
  size_t n = 0;
  for (const Value &v : values) {
    switch (v.tag()) {
      case Tag::kDecimal:
      case Tag::kString:
      case Tag::kBytes:
      case Tag::kJson:
        n += 5 + v.as_text().size();
        break;
      case Tag::kVector:
        n += 5 + wire::kVectorAlignment + v.as_vector().size() * sizeof(float);
        break;
      default:
        n += 1 + 16;  // numbers and temporals
    }
  }
  return n;
}

}  // namespace

void Writer::str(std::string_view s) {
  u32(static_cast<uint32_t>(s.size()));
  raw(s.data(), s.size());
}

void Writer::zeros(size_t n) {
  static const char kZeros[wire::kVectorAlignment] = {};
  raw(kZeros, n);
}

void Writer::i64_value(int64_t v) {
  u8(static_cast<uint8_t>(Tag::kI64));
  i64(v);
}

void Writer::u64_value(uint64_t v) {
  u8(static_cast<uint8_t>(Tag::kU64));
  u64(v);
}

void Writer::f64_value(double v) {
  u8(static_cast<uint8_t>(Tag::kF64));
  f64(v);
}

void Writer::f32_value(float v) {
  u8(static_cast<uint8_t>(Tag::kF32));
  f32(v);
}

void Writer::text_value(Tag tag, std::string_view s) {
  u8(static_cast<uint8_t>(tag));
  str(s);
}

void Writer::temporal_value(Tag tag, const Temporal &t) {
  u8(static_cast<uint8_t>(tag));
  u16(t.year);
  u8(t.month);
  u8(t.day);
  u32(t.hour);
  u8(t.minute);
  u8(t.second);
  u32(t.microsecond);
  flag(t.negative);
}

void Writer::timestamp_value(int64_t microseconds) {
  u8(static_cast<uint8_t>(Tag::kTimestamp));
  i64(microseconds);
}

void Writer::vector_value(const void *data, uint32_t dims) {
  u8(static_cast<uint8_t>(Tag::kVector));
  u32(dims);
  zeros(vector_padding(position()));
  raw(data, size_t{dims} * sizeof(float));
}

void Writer::value(const Value &v) {
  switch (v.tag()) {
    case Tag::kNull:
      null_value();
      break;
    case Tag::kI64:
      i64_value(v.as_i64());
      break;
    case Tag::kU64:
      u64_value(v.as_u64());
      break;
    case Tag::kF64:
      f64_value(v.as_f64());
      break;
    case Tag::kF32:
      f32_value(v.as_f32());
      break;
    case Tag::kDecimal:
    case Tag::kString:
    case Tag::kBytes:
    case Tag::kJson:
      text_value(v.tag(), v.as_text());
      break;
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
      temporal_value(v.tag(), v.as_temporal());
      break;
    case Tag::kTimestamp:
      timestamp_value(v.as_timestamp());
      break;
    case Tag::kVector: {
      const auto &f = v.as_vector();
      vector_value(f.data(), static_cast<uint32_t>(f.size()));
      break;
    }
  }
}

// ---------------------------------------------------------------- Reader

bool Reader::fail(std::string what) {
  if (m_error.empty())
    m_error = std::move(what) + " at offset " + std::to_string(offset());
  m_pos = m_end;  // every later read fails too
  return false;
}

bool Reader::fixed(void *out, size_t n) {
  if (!ok()) return false;
  if (remaining() < n) return fail("message truncated");
  std::memcpy(out, m_pos, n);
  m_pos += n;
  return true;
}

bool Reader::flag(bool *v) {
  uint8_t b;
  if (!u8(&b)) return false;
  if (b > 1) return fail("bad boolean");
  *v = b == 1;
  return true;
}

bool Reader::text(std::string *s, bool utf8, size_t max) {
  uint32_t n;
  if (!u32(&n)) return false;
  if (remaining() < n) return fail("string length beyond the message");
  if (n > max) return fail("string longer than its limit");
  std::string_view v(reinterpret_cast<const char *>(m_pos), n);
  if (utf8 && !is_valid_utf8(v)) return fail("invalid UTF-8");
  s->assign(v);
  m_pos += n;
  return true;
}

bool Reader::count(uint32_t *n, size_t min_size, uint32_t max) {
  if (!u32(n)) return false;
  if (min_size > 0 && *n > remaining() / min_size)
    return fail("element count beyond the message");
  if (*n > max) return fail("too many elements");
  return true;
}

bool Reader::finish() {
  if (!ok()) return false;
  if (remaining() != 0) return fail("trailing bytes");
  return true;
}

bool Reader::temporal(Tag tag, Temporal *t) {
  if (remaining() < kTemporalSize) return fail("message truncated");
  if (!u16(&t->year) || !u8(&t->month) || !u8(&t->day) || !u32(&t->hour) ||
      !u8(&t->minute) || !u8(&t->second) || !u32(&t->microsecond) ||
      !flag(&t->negative))
    return false;
  if (!is_valid_temporal(tag, *t))
    return fail(std::string("out of range ") + tag_name(tag));
  return true;
}

bool Reader::vector(std::vector<float> *v) {
  uint32_t n;
  if (!u32(&n)) return false;
  if (n == 0 || n > wire::kMaxVectorDimensions)
    return fail("bad VECTOR dimensions");
  size_t pad = vector_padding(offset());
  if (remaining() < pad) return fail("message truncated");
  for (size_t i = 0; i < pad; i++)
    if (m_pos[i] != 0) return fail("non-zero VECTOR padding");
  m_pos += pad;
  if (remaining() / sizeof(float) < n) return fail("message truncated");
  v->resize(n);
  std::memcpy(v->data(), m_pos, n * sizeof(float));
  m_pos += n * sizeof(float);
  return true;
}

bool Reader::value(Value *v) {
  Tag tag;
  if (!get_tag(*this, &tag)) return false;
  switch (tag) {
    case Tag::kNull:
      *v = Value::null();
      return true;
    case Tag::kI64: {
      int64_t x;
      if (!i64(&x)) return false;
      *v = Value::i64(x);
      return true;
    }
    case Tag::kU64: {
      uint64_t x;
      if (!u64(&x)) return false;
      *v = Value::u64(x);
      return true;
    }
    case Tag::kF64: {
      double x;
      if (!f64(&x)) return false;
      *v = Value::f64(x);
      return true;
    }
    case Tag::kF32: {
      float x;
      if (!f32(&x)) return false;
      *v = Value::f32(x);
      return true;
    }
    case Tag::kDecimal: {
      std::string s;
      if (!bytes(&s)) return false;
      if (!is_valid_decimal(s)) return fail("bad DECIMAL");
      *v = Value::decimal(std::move(s));
      return true;
    }
    case Tag::kString:
    case Tag::kJson: {
      std::string s;
      if (!lstr(&s)) return false;
      *v = tag == Tag::kString ? Value::string(std::move(s))
                               : Value::json(std::move(s));
      return true;
    }
    case Tag::kBytes: {
      std::string s;
      if (!bytes(&s)) return false;
      *v = Value::bytes(std::move(s));
      return true;
    }
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime: {
      Temporal t;
      if (!temporal(tag, &t)) return false;
      *v = Value::temporal(tag, t);
      return true;
    }
    case Tag::kTimestamp: {
      int64_t us;
      if (!i64(&us)) return false;
      if (!is_valid_timestamp(us)) return fail("out of range TIMESTAMP");
      *v = Value::timestamp(us);
      return true;
    }
    case Tag::kVector: {
      std::vector<float> f;
      if (!vector(&f)) return false;
      *v = Value::vector(std::move(f));
      return true;
    }
  }
  return fail("unknown value tag");  // not reached: get_tag checked it
}

// ---------------------------------------------------------------- messages

void encode(const ReadyMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u32(m.version);
  w.str(m.info);
  w.u64(m.features);
  w.str(m.worker_class);
  w.u32(static_cast<uint32_t>(m.languages.size()));
  for (const auto &l : m.languages) w.str(l);
  w.str(m.runtime);
}

void encode_failed_ready(uint32_t version, std::string_view info,
                         std::string *out) {
  out->clear();
  Writer w(out);
  w.u32(version);
  w.str(info);
}

bool decode(std::string_view body, ReadyMsg *m, std::string *error) {
  return decode_with(body, Sender::kWorker, m, error,
                     [](Reader &r, ReadyMsg *x) {
    uint32_t n;
    if (!r.u32(&x->version) || !r.str(&x->info) || !r.u64(&x->features) ||
        !r.str(&x->worker_class))
      return false;
    if (!is_valid_identifier(x->worker_class))
      return r.fail("bad worker class");
    if (!r.count(&n, kMinStr, wire::kMaxLanguages)) return false;
    if (n == 0) return r.fail("no languages");
    x->languages.clear();
    x->languages.resize(n);
    for (size_t i = 0; i < n; i++) {
      std::string &l = x->languages[i];
      if (!r.str(&l)) return false;
      if (!is_valid_identifier(l)) return r.fail("bad language identifier");
      if (std::find(x->languages.begin(), x->languages.begin() + i, l) !=
          x->languages.begin() + i)
        return r.fail("a language is listed twice");
    }
    return r.str(&x->runtime);
  });
}

bool decode_ready_version(std::string_view body, uint32_t *version,
                          std::string *error) {
  Reader r(body);
  if (r.u32(version)) return true;
  if (error != nullptr) *error = r.error();
  return false;
}

bool decode_ready_head(std::string_view body, uint32_t *version,
                       std::string *info, std::string *error) {
  Reader r(body);
  if (r.u32(version) && r.str(info)) return true;
  if (error != nullptr) *error = r.error();
  return false;
}

void encode(const PrepareMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u64(m.routine_id);
  w.str(m.language);
  w.str(m.name);
  w.str(m.entry);
  w.str(m.source);
  w.u32(static_cast<uint32_t>(m.params.size()));
  for (const Param &p : m.params) {
    w.str(p.name);
    w.u8(static_cast<uint8_t>(p.mode));
    put_param_type(w, p.type);
  }
  w.flag(m.returns.has_value());
  if (m.returns) put_param_type(w, *m.returns);
  w.u32(static_cast<uint32_t>(m.imports.size()));
  for (const Import &i : m.imports) {
    w.str(i.schema);
    w.str(i.name);
    w.str(i.alias);
  }
  w.str(m.schema);
  w.flag(m.check);
  w.u64(m.library_epoch);
}

bool decode(std::string_view body, PrepareMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error,
                     [](Reader &r, PrepareMsg *x) {
    uint32_t n;
    if (!r.u64(&x->routine_id) || !r.str(&x->language) || !r.str(&x->name) ||
        !r.str(&x->entry) || !r.str(&x->source) ||
        !r.count(&n, kMinParam, wire::kMaxParams))
      return false;
    x->params.clear();
    x->params.resize(n);
    for (Param &p : x->params) {
      uint8_t mode;
      if (!r.str(&p.name) || !r.u8(&mode)) return false;
      if (mode > static_cast<uint8_t>(ParamMode::kInOut))
        return r.fail("bad parameter mode");
      p.mode = static_cast<ParamMode>(mode);
      if (!get_param_type(r, &p.type)) return false;
    }
    bool has_return;
    if (!r.flag(&has_return)) return false;
    if (has_return) {
      x->returns.emplace();
      if (!get_param_type(r, &*x->returns)) return false;
    }
    if (!r.count(&n, 3 * kMinStr, wire::kMaxImports)) return false;
    x->imports.clear();
    x->imports.resize(n);
    for (Import &i : x->imports)
      if (!r.str(&i.schema) || !r.str(&i.name) || !r.str(&i.alias))
        return false;
    return r.str(&x->schema) && r.flag(&x->check) && r.u64(&x->library_epoch);
  });
}

void encode(const ParseLibraryMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.str(m.language);
  w.str(m.name);
  w.str(m.body);
}

bool decode(std::string_view body, ParseLibraryMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error,
                     [](Reader &r, ParseLibraryMsg *x) {
    return r.str(&x->language) && r.str(&x->name) && r.str(&x->body);
  });
}

void encode(const LibraryMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.str(m.schema);
  w.str(m.name);
  w.u64(m.known_version);
}

bool decode(std::string_view body, LibraryMsg *m, std::string *error) {
  return decode_with(body, Sender::kWorker, m, error,
                     [](Reader &r, LibraryMsg *x) {
    if (!r.str(&x->schema) || !r.str(&x->name) || !r.u64(&x->known_version))
      return false;
    if (!is_valid_library_name(x->schema) || !is_valid_library_name(x->name))
      return r.fail("bad library name");
    return true;
  });
}

void encode(const LibraryResultMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u8(static_cast<uint8_t>(m.status));
  w.str(m.language);
  w.str(m.body);
  w.u64(m.version);
  w.str(m.message);
}

bool decode(std::string_view body, LibraryResultMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error,
                     [](Reader &r, LibraryResultMsg *x) {
    uint8_t status;
    if (!r.u8(&status)) return false;
    if (status > wire::kMaxLibraryStatus) return r.fail("bad library status");
    x->status = static_cast<wire::LibraryStatus>(status);
    return r.str(&x->language) && r.str(&x->body) && r.u64(&x->version) &&
           r.str(&x->message);
  });
}

void encode(const CallMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u64(m.routine_id);
  w.u64(m.library_epoch);
  put_values(w, m.args);
}

bool decode(std::string_view body, CallMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error, [](Reader &r, CallMsg *x) {
    return r.u64(&x->routine_id) && r.u64(&x->library_epoch) &&
           get_values(r, &x->args);
  });
}

void encode(const ResultMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.flag(m.ret.has_value());
  if (m.ret) w.value(*m.ret);
  w.u32(static_cast<uint32_t>(m.out_params.size()));
  for (const auto &[index, v] : m.out_params) {
    w.u32(index);
    w.value(v);
  }
  w.str(m.console_out);
  w.str(m.console_err);
  w.flag(m.console_cleared);
  w.u32(static_cast<uint32_t>(m.findings.size()));
  for (const std::string &f : m.findings) w.str(f);
  w.flag(m.error.has_value());
  if (m.error) put_error(w, *m.error);
}

bool decode(std::string_view body, ResultMsg *m, std::string *error) {
  return decode_with(body, Sender::kWorker, m, error,
                     [](Reader &r, ResultMsg *x) {
    bool has;
    if (!r.flag(&has)) return false;
    if (has) {
      x->ret.emplace();
      if (!r.value(&*x->ret)) return false;
    }
    uint32_t n;
    if (!r.count(&n, kMinOutParam, wire::kMaxValues)) return false;
    x->out_params.clear();
    x->out_params.resize(n);
    for (auto &[index, v] : x->out_params)
      if (!r.u32(&index) || !r.value(&v)) return false;
    if (!r.bytes(&x->console_out) || !r.bytes(&x->console_err) ||
        !r.flag(&x->console_cleared) ||
        !r.count(&n, kMinStr, wire::kMaxFindings))
      return false;
    x->findings.clear();
    x->findings.resize(n);
    for (std::string &f : x->findings)
      if (!r.str(&f, wire::kMaxFinding)) return false;
    if (!r.flag(&has)) return false;
    if (has) {
      x->error.emplace();
      if (!get_error(r, &*x->error)) return false;
    }
    return true;
  });
}

void encode(const SqlMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u8(static_cast<uint8_t>(m.action));
  w.u64(m.stmt_id);
  w.str(m.statement);
  put_values(w, m.params);
  w.u8(m.flags);
  w.u32(m.max_rows);
  w.u32(m.row_width);
}

bool decode(std::string_view body, SqlMsg *m, std::string *error) {
  return decode_with(body, Sender::kWorker, m, error, [](Reader &r, SqlMsg *x) {
    using wire::SqlAction;
    uint8_t action;
    if (!r.u8(&action)) return false;
    if (action > wire::kMaxSqlAction) return r.fail("bad SQL action");
    x->action = static_cast<SqlAction>(action);
    if (!r.u64(&x->stmt_id) || !r.str(&x->statement) ||
        !get_values(r, &x->params) || !r.u8(&x->flags) ||
        !r.u32(&x->max_rows) || !r.u32(&x->row_width))
      return false;
    if ((x->flags & ~wire::kKnownSqlFlags) != 0)
      return r.fail("unknown SQL flags");
    // The fields each action uses (§10.2); the others are empty or 0.
    const SqlAction a = x->action;
    const bool text = a == SqlAction::kExecute || a == SqlAction::kPrepare;
    const bool params = a == SqlAction::kExecute ||
                        a == SqlAction::kExecutePrepared ||
                        a == SqlAction::kExecuteMany;
    const bool max_rows = a == SqlAction::kExecute ||
                          a == SqlAction::kExecutePrepared ||
                          a == SqlAction::kFetch || a == SqlAction::kNextResult;
    if ((!text && (!x->statement.empty() || x->flags != 0)) ||
        (!params && !x->params.empty()) || (!max_rows && x->max_rows != 0) ||
        (a != SqlAction::kExecuteMany && x->row_width != 0))
      return r.fail("a field the SQL action does not use is set");
    return true;
  });
}

void encode(const SqlResultMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  switch (m.kind) {
    case SqlResultKind::kError:
      w.u8(static_cast<uint8_t>(m.kind));
      w.u32(m.sql_errno);
      w.str(m.sqlstate);
      w.str(m.message);
      w.u32(m.failed_row);
      w.u64(m.error_id);
      return;
    case SqlResultKind::kOk:
      w.u8(static_cast<uint8_t>(m.kind));
      w.u64(m.affected_rows);
      w.u64(m.insert_id);
      put_warnings(w, m.warnings);
      w.flag(m.more_results);
      w.u32(m.param_count);
      w.u64(m.exec_us);
      return;
    case SqlResultKind::kRows: {
      const size_t count_at = put_rows_head(w, m.columns, m.total_rows);
      const size_t ncols =
          m.columns.empty() ? size_t{m.ncolumns} : m.columns.size();
      const uint32_t rows =
          static_cast<uint32_t>(ncols == 0 ? 0 : m.rows.size() / ncols);
      memcpy(out->data() + count_at, &rows, sizeof rows);
      // Rows with VECTORs or long text make bodies of many MB: one
      // allocation instead of a growing string's copies and page faults.
      out->reserve(out->size() + encoded_size(m.rows) + 64);
      for (const Value &v : m.rows) w.value(v);
      put_rows_tail(w, m.more, m.more_results, m.warnings, m.exec_us);
      return;
    }
    case SqlResultKind::kEnd:
      w.u8(static_cast<uint8_t>(m.kind));
      return;
  }
}

size_t put_rows_head(Writer &w, const std::vector<Column> &columns,
                     uint64_t total_rows) {
  w.u8(static_cast<uint8_t>(SqlResultKind::kRows));
  w.u32(static_cast<uint32_t>(columns.size()));
  for (const Column &c : columns) {
    w.str(c.name);
    w.str(c.table);
    put_param_type(w, c.type);
    w.str(c.org_name);
    w.str(c.schema);
    w.str(c.org_table);
    w.str(c.charset);
    w.str(c.collation);
    w.u32(c.flags);
  }
  w.u64(total_rows);
  const size_t count_at = w.position();
  w.u32(0);  // the row count, patched by the caller
  return count_at;
}

void put_rows_tail(Writer &w, bool more, bool more_results,
                   const std::vector<Warning> &warnings, uint64_t exec_us) {
  w.flag(more);
  w.flag(more_results);
  put_warnings(w, warnings);
  w.u64(exec_us);
}

bool decode(std::string_view body, SqlResultMsg *m, std::string *error,
            uint32_t set_columns) {
  return decode_with(body, Sender::kHost, m, error,
                     [set_columns](Reader &r, SqlResultMsg *x) {
    uint8_t kind;
    if (!r.u8(&kind)) return false;
    if (kind > wire::kMaxSqlResultKind) return r.fail("bad SQL result kind");
    x->kind = static_cast<SqlResultKind>(kind);
    switch (x->kind) {
      case SqlResultKind::kError:
        if (!r.u32(&x->sql_errno) || !r.str(&x->sqlstate) ||
            !r.str(&x->message) || !r.u32(&x->failed_row) ||
            !r.u64(&x->error_id))
          return false;
        return is_valid_sqlstate(x->sqlstate) || r.fail("bad SQLSTATE");
      case SqlResultKind::kOk:
        return r.u64(&x->affected_rows) && r.u64(&x->insert_id) &&
               get_warnings(r, &x->warnings) && r.flag(&x->more_results) &&
               r.u32(&x->param_count) && r.u64(&x->exec_us);
      case SqlResultKind::kRows: {
        uint32_t ncols, nrows;
        if (!r.count(&ncols, kMinColumn, wire::kMaxColumns)) return false;
        x->columns.clear();
        x->columns.resize(ncols);
        for (Column &c : x->columns)
          if (!r.str(&c.name) || !r.str(&c.table) ||
              !get_param_type(r, &c.type) || !r.str(&c.org_name) ||
              !r.str(&c.schema) || !r.str(&c.org_table) ||
              !r.str(&c.charset) || !r.str(&c.collation) || !r.u32(&c.flags))
            return false;
        if (ncols == 0) ncols = x->ncolumns = set_columns;
        if (!r.u64(&x->total_rows) || !r.u32(&nrows)) return false;
        const uint64_t nvalues = uint64_t{nrows} * ncols;
        if (ncols == 0 && nrows != 0) return r.fail("rows without columns");
        if (nvalues > r.remaining() / kMinValue)
          return r.fail("row count beyond the message");
        x->rows.clear();
        x->rows.resize(nvalues);
        for (Value &v : x->rows)
          if (!r.value(&v)) return false;
        return r.flag(&x->more) && r.flag(&x->more_results) &&
               get_warnings(r, &x->warnings) && r.u64(&x->exec_us);
      }
      case SqlResultKind::kEnd:
        return true;
    }
    return false;
  });
}

void encode(const DropMsg &m, std::string *out) {
  out->clear();
  Writer(out).u64(m.routine_id);
}

bool decode(std::string_view body, DropMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error, [](Reader &r, DropMsg *x) {
    return r.u64(&x->routine_id);
  });
}

void encode(const OptionsMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.str(m.time_zone);
  w.u32(static_cast<uint32_t>(m.settings.size()));
  for (const Setting &s : m.settings) {
    w.str(s.name);
    w.str(s.value);
  }
}

bool decode(std::string_view body, OptionsMsg *m, std::string *error) {
  return decode_with(body, Sender::kHost, m, error,
                     [](Reader &r, OptionsMsg *x) {
    uint32_t n;
    if (!r.str(&x->time_zone) ||
        !r.count(&n, 2 * kMinStr, wire::kMaxSettings))
      return false;
    x->settings.clear();
    x->settings.resize(n);
    for (Setting &s : x->settings)
      if (!r.str(&s.name) || !r.str(&s.value)) return false;
    const char *why = nullptr;
    return !options_problem(*x, &why) || r.fail(why);
  });
}

// ---------------------------------------------------------------- chunks

bool check_worker_header(const wire::MessageHeader &h, uint32_t arena_size,
                         std::string *error) {
  const bool result = h.op == static_cast<uint32_t>(wire::Op::kResult);
  const bool ready = h.op == static_cast<uint32_t>(wire::Op::kReady);
  const bool more = (h.flags & wire::kMore) != 0;
  const char *why = nullptr;
  if (!wire::is_assigned_op(h.op))
    why = "unknown op";
  else if (result ? h.status > wire::kMaxStatus
                  : ready ? h.status != static_cast<uint32_t>(wire::Status::kOk) &&
                                h.status != static_cast<uint32_t>(wire::Status::kInternal)
                          : h.status != 0)
    why = "bad status";
  else if ((h.flags & ~wire::kKnownMessageFlags) != 0)
    why = "unknown flags";
  else if (h.reserved != 0)
    why = "reserved header field is not 0";
  else if (h.len > arena_size)
    why = "chunk larger than the arena";
  else if (more && h.len != arena_size)
    why = "a chunk before the last does not fill the arena";
  else if (h.total_len > wire::kMaxWorkerMessage)
    why = "message too large";
  else if (h.op == static_cast<uint32_t>(wire::Op::kContinue) &&
           (h.flags != 0 || h.len != 0 || h.total_len != 0))
    why = "bad CONTINUE";
  if (why == nullptr) return true;
  if (error != nullptr) *error = why;
  return false;
}

void Assembler::reset() {
  m_started = false;
  m_discard = false;
  m_received = 0;
  m_first = {};
  m_body.clear();
  m_error.clear();
}

Assembler::Step Assembler::fail(std::string what) {
  m_error = std::move(what);
  return Step::kError;
}

void Assembler::discard_beyond(size_t keep) {
  m_discard = true;
  if (m_body.size() > keep) m_body.resize(keep);
}

Assembler::Step Assembler::add(const wire::MessageHeader &h,
                               const void *chunk) {
  if (!m_error.empty()) return Step::kError;
  if (h.len > m_arena_size) return fail("chunk larger than the arena");
  const bool more = (h.flags & wire::kMore) != 0;
  if (!m_started) {
    if (h.total_len > m_max_message) return fail("message too large");
    if (h.len > h.total_len) return fail("chunk larger than the message");
    m_first = h;
    m_started = true;
    m_body.clear();
  } else if (h.op != m_first.op || h.status != m_first.status ||
             h.depth != m_first.depth || h.call_seq != m_first.call_seq ||
             h.total_len != m_first.total_len) {
    return fail("chunk does not continue the message");
  }
  if (h.len > m_first.total_len - m_received)
    return fail("chunks exceed the message length");
  // Every chunk but the last fills the arena, and a message that fits the
  // arena is one chunk: a sender cannot stretch a message into many turns.
  if (more && h.len != m_arena_size)
    return fail("a chunk before the last does not fill the arena");
  if (more && m_received + h.len == m_first.total_len)
    return fail("more chunks announced after the last byte");
  if (!more && m_received + h.len != m_first.total_len)
    return fail("message truncated");
  if (!m_discard) {
    // The second chunk shows that the first was accepted: now the whole
    // body is allocated at once.
    if (m_received != 0 && m_body.capacity() < m_first.total_len)
      m_body.reserve(m_first.total_len);
    m_body.append(static_cast<const char *>(chunk), h.len);
  }
  m_received += h.len;
  return more ? Step::kNeedMore : Step::kDone;
}

}  // namespace mle
