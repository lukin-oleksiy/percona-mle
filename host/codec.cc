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

#include <bit>
#include <cstring>

namespace mle {

static_assert(std::endian::native == std::endian::little,
              "the MLE wire protocol is little-endian; add byte swapping");

namespace {

// Smallest encodings, used to bound element counts.
constexpr size_t kMinValue = 1;                      // tag only (NULL)
constexpr size_t kMinStr = 4;                        // empty string
constexpr size_t kMinParamType = 1 + 1 + 4 + 4 + 4 + kMinStr;  // tag .. name
constexpr size_t kMinParam = kMinStr + 1 + kMinParamType;
constexpr size_t kMinColumn = 2 * kMinStr + kMinParamType;
constexpr size_t kMinFrame = 2 * kMinStr + 4 + 4 + 4;
constexpr size_t kMinWarning = 4 + 4 + kMinStr;
constexpr size_t kMinOutParam = 4 + kMinValue;
constexpr size_t kTemporalSize = 2 + 1 + 1 + 4 + 1 + 1 + 4 + 1;
constexpr size_t kMaxDecimalChars = 100;

size_t vector_padding(size_t offset) {
  return (wire::kVectorAlignment - offset % wire::kVectorAlignment) %
         wire::kVectorAlignment;
}

void put_param_type(Writer &w, const ParamType &t) {
  w.u8(static_cast<uint8_t>(t.tag));
  w.u8(t.is_unsigned ? 1 : 0);
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
  if (!r.count(&n, kMinValue)) return false;
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
  if (!r.count(&n, kMinWarning)) return false;
  warnings->clear();
  warnings->resize(n);
  for (Warning &x : *warnings)
    if (!r.u32(&x.level) || !r.u32(&x.code) || !r.str(&x.message)) return false;
  return true;
}

bool valid_sqlstate(std::string_view s) {
  if (s.empty()) return true;
  if (s.size() != 5) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
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
  }
  w.u32(e.sql_errno);
  w.str(e.sqlstate);
}

bool get_error(Reader &r, Error *e) {
  uint32_t n;
  if (!r.str(&e->language) || !r.str(&e->message) || !r.count(&n, kMinFrame))
    return false;
  if (n > wire::kMaxStackFrames) return r.fail("too many stack frames");
  e->stack.clear();
  e->stack.resize(n);
  for (StackFrame &f : e->stack)
    if (!r.str(&f.function) || !r.str(&f.file) || !r.u32(&f.line) ||
        !r.u32(&f.column) || !r.u32(&f.end_column))
      return false;
  if (!r.u32(&e->sql_errno) || !r.str(&e->sqlstate)) return false;
  if (!valid_sqlstate(e->sqlstate)) return r.fail("bad SQLSTATE");
  return true;
}

template <class M>
bool decode_with(std::string_view body, M *m, std::string *error,
                 bool (*fn)(Reader &, M *)) {
  Reader r(body);
  *m = M();
  if (fn(r, m) && r.finish()) return true;
  if (error != nullptr) *error = r.error();
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
  if (s.empty() || s.size() > kMaxDecimalChars) return false;
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
    case Tag::kTimestamp:
      return t.year <= 9999 && t.month <= 12 && t.day <= 31 && t.hour <= 23 &&
             time_ok && !t.negative;
    case Tag::kTime: {
      if (t.year != 0 || t.month != 0 || t.day != 0 || !time_ok) return false;
      if (t.hour < wire::kMaxTimeHour) return true;
      // 838:59:59.000000 is the largest TIME.
      return t.hour == wire::kMaxTimeHour && t.minute == 59 && t.second == 59 &&
             t.microsecond == 0;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------- Writer

void Writer::str(std::string_view s) {
  u32(static_cast<uint32_t>(s.size()));
  raw(s.data(), s.size());
}

void Writer::value(const Value &v) {
  u8(static_cast<uint8_t>(v.tag()));
  switch (v.tag()) {
    case Tag::kNull:
      break;
    case Tag::kI64:
      u64(static_cast<uint64_t>(v.as_i64()));
      break;
    case Tag::kU64:
      u64(v.as_u64());
      break;
    case Tag::kF64:
      f64(v.as_f64());
      break;
    case Tag::kF32:
      f32(v.as_f32());
      break;
    case Tag::kDecimal:
    case Tag::kString:
    case Tag::kBytes:
    case Tag::kJson:
      str(v.as_text());
      break;
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
    case Tag::kTimestamp: {
      const Temporal &t = v.as_temporal();
      u16(t.year);
      u8(t.month);
      u8(t.day);
      u32(t.hour);
      u8(t.minute);
      u8(t.second);
      u32(t.microsecond);
      u8(t.negative ? 1 : 0);
      break;
    }
    case Tag::kVector: {
      const auto &f = v.as_vector();
      u32(static_cast<uint32_t>(f.size()));
      m_out->append(vector_padding(m_out->size()), '\0');
      raw(f.data(), f.size() * sizeof(float));
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

bool Reader::text(std::string *s, bool utf8) {
  uint32_t n;
  if (!u32(&n)) return false;
  if (remaining() < n) return fail("string length beyond the message");
  std::string_view v(reinterpret_cast<const char *>(m_pos), n);
  if (utf8 && !is_valid_utf8(v)) return fail("invalid UTF-8");
  s->assign(v);
  m_pos += n;
  return true;
}

bool Reader::count(uint32_t *n, size_t min_size) {
  if (!u32(n)) return false;
  if (min_size > 0 && *n > remaining() / min_size)
    return fail("element count beyond the message");
  return true;
}

bool Reader::finish() {
  if (!ok()) return false;
  if (remaining() != 0) return fail("trailing bytes");
  return true;
}

bool Reader::temporal(Tag tag, Temporal *t) {
  uint8_t negative;
  if (!u16(&t->year) || !u8(&t->month) || !u8(&t->day) || !u32(&t->hour) ||
      !u8(&t->minute) || !u8(&t->second) || !u32(&t->microsecond) ||
      !u8(&negative))
    return false;
  if (negative > 1) return fail("bad boolean");
  t->negative = negative == 1;
  if (!is_valid_temporal(tag, *t))
    return fail(std::string("out of range ") + tag_name(tag));
  return true;
}

bool Reader::vector(std::vector<float> *v) {
  uint32_t n;
  if (!u32(&n)) return false;
  if (n > wire::kMaxVectorDimensions) return fail("VECTOR too large");
  size_t pad = vector_padding(offset());
  if (remaining() < pad) return fail("message truncated");
  for (size_t i = 0; i < pad; i++)
    if (m_pos[i] != 0) return fail("non-zero VECTOR padding");
  m_pos += pad;
  if (remaining() / sizeof(float) < n) return fail("message truncated");
  v->resize(n);
  if (n > 0) std::memcpy(v->data(), m_pos, n * sizeof(float));
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
      uint64_t x;
      if (!u64(&x)) return false;
      *v = Value::i64(static_cast<int64_t>(x));
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
      if (!str(&s)) return false;
      if (!is_valid_decimal(s)) return fail("bad DECIMAL");
      *v = Value::decimal(std::move(s));
      return true;
    }
    case Tag::kString:
    case Tag::kJson: {
      std::string s;
      if (!str(&s)) return false;
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
    case Tag::kDatetime:
    case Tag::kTimestamp: {
      if (remaining() < kTemporalSize) return fail("message truncated");
      Temporal t;
      if (!temporal(tag, &t)) return false;
      *v = Value::temporal(tag, t);
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
  w.str(m.worker_class);
  w.u32(static_cast<uint32_t>(m.languages.size()));
  for (const auto &l : m.languages) w.str(l);
  w.str(m.runtime);
  w.str(m.info);
}

bool decode(std::string_view body, ReadyMsg *m, std::string *error) {
  return decode_with<ReadyMsg>(body, m, error, [](Reader &r, ReadyMsg *x) {
    uint32_t n;
    if (!r.u32(&x->version) || !r.str(&x->worker_class) ||
        !r.count(&n, kMinStr))
      return false;
    x->languages.clear();
    x->languages.resize(n);
    for (auto &l : x->languages)
      if (!r.str(&l)) return false;
    return r.str(&x->runtime) && r.str(&x->info);
  });
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
  w.u8(m.returns ? 1 : 0);
  if (m.returns) put_param_type(w, *m.returns);
}

bool decode(std::string_view body, PrepareMsg *m, std::string *error) {
  return decode_with<PrepareMsg>(body, m, error, [](Reader &r, PrepareMsg *x) {
    uint32_t n;
    if (!r.u64(&x->routine_id) || !r.str(&x->language) || !r.str(&x->name) ||
        !r.str(&x->entry) ||
        !r.str(&x->source) || !r.count(&n, kMinParam))
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
    return true;
  });
}

void encode(const CallMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u64(m.routine_id);
  w.u32(m.events);
  put_values(w, m.args);
}

bool decode(std::string_view body, CallMsg *m, std::string *error) {
  return decode_with<CallMsg>(body, m, error, [](Reader &r, CallMsg *x) {
    if (!r.u64(&x->routine_id) || !r.u32(&x->events)) return false;
    if ((x->events & ~wire::kKnownEvents) != 0) return r.fail("unknown events");
    return get_values(r, &x->args);
  });
}

void encode(const ResultMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u8(m.ret ? 1 : 0);
  if (m.ret) w.value(*m.ret);
  w.u32(static_cast<uint32_t>(m.out_params.size()));
  for (const auto &[index, v] : m.out_params) {
    w.u32(index);
    w.value(v);
  }
  w.str(m.console_out);
  w.str(m.console_err);
  w.u8(m.error ? 1 : 0);
  if (m.error) put_error(w, *m.error);
}

bool decode(std::string_view body, ResultMsg *m, std::string *error) {
  return decode_with<ResultMsg>(body, m, error, [](Reader &r, ResultMsg *x) {
    bool has;
    if (!r.flag(&has)) return false;
    if (has) {
      x->ret.emplace();
      if (!r.value(&*x->ret)) return false;
    }
    uint32_t n;
    if (!r.count(&n, kMinOutParam)) return false;
    x->out_params.clear();
    x->out_params.resize(n);
    for (auto &[index, v] : x->out_params)
      if (!r.u32(&index) || !r.value(&v)) return false;
    // Console output may hold any bytes the guest printed.
    if (!r.bytes(&x->console_out) || !r.bytes(&x->console_err) || !r.flag(&has))
      return false;
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
  w.str(m.statement);
  put_values(w, m.params);
  w.u32(m.max_rows);
}

bool decode(std::string_view body, SqlMsg *m, std::string *error) {
  return decode_with<SqlMsg>(body, m, error, [](Reader &r, SqlMsg *x) {
    return r.str(&x->statement) && get_values(r, &x->params) &&
           r.u32(&x->max_rows);
  });
}

void encode(const SqlResultMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u8(static_cast<uint8_t>(m.kind));
  switch (m.kind) {
    case SqlResultKind::kError:
      w.u32(m.sql_errno);
      w.str(m.sqlstate);
      w.str(m.message);
      return;
    case SqlResultKind::kOk:
      w.u64(m.affected_rows);
      w.u64(m.insert_id);
      put_warnings(w, m.warnings);
      return;
    case SqlResultKind::kRows: {
      w.u32(static_cast<uint32_t>(m.columns.size()));
      for (const Column &c : m.columns) {
        w.str(c.name);
        w.str(c.table);
        put_param_type(w, c.type);
      }
      const size_t ncols = m.columns.size();
      w.u32(static_cast<uint32_t>(ncols == 0 ? 0 : m.rows.size() / ncols));
      for (const Value &v : m.rows) w.value(v);
      w.u8(m.more ? 1 : 0);
      put_warnings(w, m.warnings);
      return;
    }
  }
}

bool decode(std::string_view body, SqlResultMsg *m, std::string *error) {
  return decode_with<SqlResultMsg>(
      body, m, error, [](Reader &r, SqlResultMsg *x) {
        uint8_t kind;
        if (!r.u8(&kind)) return false;
        switch (kind) {
          case static_cast<uint8_t>(SqlResultKind::kError):
            x->kind = SqlResultKind::kError;
            if (!r.u32(&x->sql_errno) || !r.str(&x->sqlstate) ||
                !r.str(&x->message))
              return false;
            return valid_sqlstate(x->sqlstate) || r.fail("bad SQLSTATE");
          case static_cast<uint8_t>(SqlResultKind::kOk):
            x->kind = SqlResultKind::kOk;
            return r.u64(&x->affected_rows) && r.u64(&x->insert_id) &&
                   get_warnings(r, &x->warnings);
          case static_cast<uint8_t>(SqlResultKind::kRows): {
            x->kind = SqlResultKind::kRows;
            uint32_t ncols, nrows;
            if (!r.count(&ncols, kMinColumn)) return false;
            x->columns.clear();
            x->columns.resize(ncols);
            for (Column &c : x->columns)
              if (!r.str(&c.name) || !r.str(&c.table) ||
                  !get_param_type(r, &c.type))
                return false;
            if (!r.u32(&nrows)) return false;
            const uint64_t nvalues = uint64_t{nrows} * ncols;
            if (ncols == 0 && nrows != 0) return r.fail("rows without columns");
            if (nvalues > r.remaining() / kMinValue)
              return r.fail("row count beyond the message");
            x->rows.clear();
            x->rows.resize(nvalues);
            for (Value &v : x->rows)
              if (!r.value(&v)) return false;
            return r.flag(&x->more) && get_warnings(r, &x->warnings);
          }
          default:
            return r.fail("bad SQL result kind");
        }
      });
}

void encode(const FetchMsg &m, std::string *out) {
  out->clear();
  Writer(out).u32(m.max_rows);
}

bool decode(std::string_view body, FetchMsg *m, std::string *error) {
  return decode_with<FetchMsg>(body, m, error, [](Reader &r, FetchMsg *x) {
    return r.u32(&x->max_rows);
  });
}

void encode(const DropMsg &m, std::string *out) {
  out->clear();
  Writer(out).u64(m.routine_id);
}

bool decode(std::string_view body, DropMsg *m, std::string *error) {
  return decode_with<DropMsg>(body, m, error, [](Reader &r, DropMsg *x) {
    return r.u64(&x->routine_id);
  });
}

void encode(const OptionsMsg &m, std::string *out) {
  out->clear();
  Writer w(out);
  w.u8(static_cast<uint8_t>(m.integer_type));
  w.u8(static_cast<uint8_t>(m.decimal_type));
  w.str(m.time_zone);
}

bool decode(std::string_view body, OptionsMsg *m, std::string *error) {
  return decode_with<OptionsMsg>(body, m, error, [](Reader &r, OptionsMsg *x) {
    uint8_t i, d;
    if (!r.u8(&i) || !r.u8(&d) || !r.str(&x->time_zone)) return false;
    if (i > wire::kMaxIntegerType) return r.fail("bad integer type");
    if (d > wire::kMaxDecimalType) return r.fail("bad decimal type");
    x->integer_type = static_cast<wire::IntegerType>(i);
    x->decimal_type = static_cast<wire::DecimalType>(d);
    return true;
  });
}

// ---------------------------------------------------------------- chunks

bool check_worker_header(const wire::MessageHeader &h, uint32_t arena_size,
                         std::string *error) {
  const char *why = nullptr;
  if (h.op == 0 || h.op > wire::kMaxOp)
    why = "unknown op";
  else if (h.status > wire::kMaxWorkerStatus)
    why = "bad status";
  else if ((h.flags & ~wire::kKnownMessageFlags) != 0)
    why = "unknown flags";
  else if (h.len > arena_size)
    why = "chunk larger than the arena";
  else if (h.total_len > wire::kMaxMessageSize)
    why = "message too large";
  if (why == nullptr) return true;
  if (error != nullptr) *error = why;
  return false;
}

void Assembler::reset() {
  m_started = false;
  m_first = {};
  m_body.clear();
  m_error.clear();
}

Assembler::Step Assembler::fail(std::string what) {
  m_error = std::move(what);
  return Step::kError;
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
    m_body.reserve(h.total_len);
  } else if (h.op != m_first.op || h.status != m_first.status ||
             h.depth != m_first.depth || h.call_seq != m_first.call_seq ||
             h.total_len != m_first.total_len) {
    return fail("chunk does not continue the message");
  }
  if (h.len > m_first.total_len - m_body.size())
    return fail("chunks exceed the message length");
  if (more && h.len == 0) return fail("empty chunk");
  m_body.append(static_cast<const char *>(chunk), h.len);
  if (more) {
    if (m_body.size() == m_first.total_len)
      return fail("more chunks announced after the last byte");
    return Step::kNeedMore;
  }
  if (m_body.size() != m_first.total_len) return fail("message truncated");
  return Step::kDone;
}

}  // namespace mle
