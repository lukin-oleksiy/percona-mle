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

#include "values.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mle::v8w {

namespace w = mle::wire;
using v8::Local;
using v8::Value;

namespace {

constexpr int64_t kMaxSafe = (int64_t{1} << 53) - 1;
constexpr const char *kTagNames[] = {"NULL", "BIGINT", "BIGINT UNSIGNED", "DOUBLE", "FLOAT", "DECIMAL",
                                     "string", "binary", "DATE", "TIME", "DATETIME", "TIMESTAMP",
                                     "JSON", "VECTOR"};
constexpr size_t kDecimalMaxDigits = 65;
constexpr size_t kDecimalMaxScale = 30;

bool safe(int64_t v) { return v >= -kMaxSafe && v <= kMaxSafe; }

std::string trim(std::string_view s) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return std::string(s);
}

/** A decimal number: sign, digits of the unscaled value (no leading zeros; "0" for zero), scale
    (Java's BigDecimal). */
struct Decimal {
  bool negative = false;
  std::string digits = "0";
  int64_t scale = 0;

  bool zero() const { return digits == "0"; }
  int64_t precision() const { return static_cast<int64_t>(digits.size()); }

  /** Java's BigDecimal(String) syntax; nullopt if it is not a number. */
  static std::optional<Decimal> parse(std::string_view s) {
    Decimal d;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) d.negative = s[i++] == '-';
    std::string all;
    int64_t frac = 0;
    bool point = false, any = false;
    for (; i < s.size(); i++) {
      const char c = s[i];
      if (c >= '0' && c <= '9') {
        all += c;
        any = true;
        if (point) frac++;
      } else if (c == '.' && !point) {
        point = true;
      } else {
        break;
      }
    }
    if (!any) return std::nullopt;
    int64_t exp = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      i++;
      bool eneg = false;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
      if (i == s.size()) return std::nullopt;
      for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9' || exp > 100000000) return std::nullopt;
        exp = exp * 10 + (s[i] - '0');
      }
      if (eneg) exp = -exp;
    }
    if (i != s.size()) return std::nullopt;
    const size_t nz = all.find_first_not_of('0');
    d.digits = nz == std::string::npos ? "0" : all.substr(nz);
    d.scale = frac - exp;
    if (d.zero()) d.negative = false;
    return d;
  }

  /** Rounds (HALF_UP) to at most `to` digits after the point. */
  void round_to(int64_t to) {
    if (scale <= to) return;
    const int64_t drop = scale - to;
    if (drop > precision()) {
      digits = "0";
      negative = false;
      scale = to;
      return;
    }
    const bool up = digits[digits.size() - drop] >= '5';
    digits.resize(digits.size() - drop);
    if (digits.empty()) digits = "0";
    scale = to;
    if (up) {
      int i = static_cast<int>(digits.size()) - 1;
      for (; i >= 0; i--) {
        if (digits[i] == '9') {
          digits[i] = '0';
        } else {
          digits[i]++;
          break;
        }
      }
      if (i < 0) digits.insert(digits.begin(), '1');
    }
    const size_t nz = digits.find_first_not_of('0');
    digits = nz == std::string::npos ? "0" : digits.substr(nz);
    if (zero()) negative = false;
  }

  std::string plain() const {
    std::string out = negative ? "-" : "";
    if (scale <= 0) {
      out += digits;
      if (!zero()) out.append(static_cast<size_t>(-scale), '0');
      return out;
    }
    std::string d = digits;
    if (static_cast<int64_t>(d.size()) <= scale) d.insert(0, static_cast<size_t>(scale) - d.size() + 1, '0');
    out += d.substr(0, d.size() - scale) + "." + d.substr(d.size() - scale);
    return out;
  }
};

}  // namespace

// ------------------------------------------------------------ strings

std::string utf8(v8::Isolate *iso, Local<Value> v) {
  if (v.IsEmpty()) return "";
  Local<v8::String> s;
  if (v->IsString()) {
    s = v.As<v8::String>();
  } else {
    const v8::TryCatch tc(iso);
    if (!v->ToString(iso->GetCurrentContext()).ToLocal(&s)) return "?";
  }
  std::string out(s->Utf8Length(iso), '\0');
  // An unpaired surrogate becomes U+FFFD (3 bytes, as Utf8Length counted it).
  s->WriteUtf8(iso, out.data(), static_cast<int>(out.size()), nullptr,
               v8::String::NO_NULL_TERMINATION | v8::String::REPLACE_INVALID_UTF8);
  return out;
}

std::optional<std::string> utf8_exact(v8::Isolate *iso, Local<v8::String> s) {
  if (!s->ContainsOnlyOneByte()) {
    const v8::String::Value units(iso, s);
    const auto high = [](uint16_t c) { return c >= 0xD800 && c <= 0xDBFF; };
    const auto low = [](uint16_t c) { return c >= 0xDC00 && c <= 0xDFFF; };
    for (int i = 0; i < units.length(); i++) {
      const uint16_t c = (*units)[i];
      if (high(c) && i + 1 < units.length() && low((*units)[i + 1]))
        i++;
      else if (high(c) || low(c))
        return std::nullopt;
    }
  }
  return utf8(iso, s);
}

Local<v8::String> v8_string(v8::Isolate *iso, std::string_view s) {
  Local<v8::String> out;
  if (s.size() > static_cast<size_t>(v8::String::kMaxLength) ||
      !v8::String::NewFromUtf8(iso, s.data(),
                               s.size() < 32 ? v8::NewStringType::kInternalized : v8::NewStringType::kNormal,
                               static_cast<int>(s.size()))
           .ToLocal(&out))
    throw ConversionError{"a text of " + std::to_string(s.size()) + " bytes is longer than a JavaScript string can be"};
  return out;
}

// ------------------------------------------------------------ ParamType

ParamType ParamType::read(Reader &r) {
  ParamType t;
  const auto tag = r.get<uint8_t>();
  if (tag > w::kMaxTag) protocol_failure("an unknown tag in a Type");
  t.tag = static_cast<w::Tag>(tag);
  t.is_unsigned = r.boolean();
  t.length = r.get<uint32_t>();
  t.precision = r.get<uint32_t>();
  t.scale = r.get<uint32_t>();
  t.type_name = std::string(r.str());
  return t;
}

std::string ParamType::sql_name() const {
  if (!type_name.empty()) return type_name;
  return kTagNames[static_cast<uint8_t>(tag)];
}

// ------------------------------------------------------------ time zones

bool Options::set_zone(std::string_view text) {
  if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
    const auto digit = [&](size_t i) { return text[i] >= '0' && text[i] <= '9'; };
    if (text.size() != 6 || !digit(1) || !digit(2) || text[3] != ':' || !digit(4) || !digit(5)) return false;
    const int h = (text[1] - '0') * 10 + (text[2] - '0'), m = (text[4] - '0') * 10 + (text[5] - '0');
    const int minutes = h * 60 + m;
    if (m > 59 || minutes > (text[0] == '-' ? 13 * 60 + 59 : 14 * 60)) return false;
    offset = std::chrono::seconds(minutes * 60 * (text[0] == '-' ? -1 : 1));
    zone = nullptr;
    return true;
  }
  try {
    zone = std::chrono::locate_zone(text);
    offset = std::chrono::seconds(0);
    return true;
  } catch (const std::runtime_error &) {
    return false;
  }
}

double Options::epoch_ms(int year, unsigned month, unsigned day, int64_t hour, unsigned minute, unsigned second,
                         unsigned micro) const {
  using namespace std::chrono;
  const local_days d{std::chrono::year(year) / std::chrono::month(month) / std::chrono::day(day)};
  const local_time<microseconds> t = d + hours(hour) + minutes(minute) + seconds(second) + microseconds(micro);
  seconds off = offset;
  if (zone != nullptr) {
    // Java's atZone: a local time in a gap moves forward by the gap (the offset before it); in an
    // overlap the earlier offset.
    const local_info info = zone->get_info(floor<seconds>(t));
    off = info.first.offset;
  }
  const auto sys = t.time_since_epoch() - off;
  // Milliseconds as Java's toEpochMilli (floor), as a double for Date.
  return static_cast<double>(floor<milliseconds>(sys).count());
}

void Options::local(double ms, int *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute,
                    unsigned *second, unsigned *micro) const {
  using namespace std::chrono;
  const sys_time<milliseconds> t{milliseconds(static_cast<int64_t>(std::floor(ms)))};
  seconds off = offset;
  if (zone != nullptr) off = zone->get_info(floor<seconds>(t)).offset;
  const auto local = t + off;
  const auto days = floor<std::chrono::days>(local);
  const year_month_day ymd{days};
  const auto rest = local - days;
  *year = static_cast<int>(ymd.year());
  *month = static_cast<unsigned>(ymd.month());
  *day = static_cast<unsigned>(ymd.day());
  *hour = static_cast<unsigned>(duration_cast<hours>(rest).count());
  *minute = static_cast<unsigned>(duration_cast<minutes>(rest).count() % 60);
  *second = static_cast<unsigned>(duration_cast<seconds>(rest).count() % 60);
  *micro = static_cast<unsigned>((rest.count() % 1000) * 1000);
}

// ------------------------------------------------------------ temporal values

bool valid_temporal(const Temporal &t) {
  const bool date = t.year <= 9999 && t.month <= 12 && t.day <= 31;
  const bool clock = t.minute <= 59 && t.second <= 59 && t.micro <= 999999;
  switch (t.tag) {
    case w::Tag::kDate:
      return date && t.hour == 0 && t.minute == 0 && t.second == 0 && t.micro == 0 && !t.negative;
    case w::Tag::kDatetime:
      return date && clock && t.hour <= 23 && !t.negative;
    case w::Tag::kTime:
      return t.year == 0 && t.month == 0 && t.day == 0 && clock && t.hour <= w::kMaxTimeHour &&
             !(t.hour == w::kMaxTimeHour && t.minute == 59 && t.second == 59 && t.micro != 0);
    default:
      return false;
  }
}

bool valid_decimal(std::string_view s) {
  if (s.empty() || s.size() > w::kMaxDecimalChars) return false;
  if (s[0] == '-') s.remove_prefix(1);
  const auto digits = [&s]() {
    size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') n++;
    s.remove_prefix(n);
    return n != 0;
  };
  if (!digits()) return false;
  if (s.empty()) return true;
  if (s[0] != '.') return false;
  s.remove_prefix(1);
  return digits() && s.empty();
}

Temporal read_temporal(Reader &r, w::Tag tag) {
  Temporal t{};
  t.tag = tag;
  t.year = r.get<uint16_t>();
  t.month = r.get<uint8_t>();
  t.day = r.get<uint8_t>();
  t.hour = r.get<uint32_t>();
  t.minute = r.get<uint8_t>();
  t.second = r.get<uint8_t>();
  t.micro = r.get<uint32_t>();
  t.negative = r.boolean();
  return t;
}

void write_temporal(Writer &wr, const Temporal &t) {
  if (!(wr.put(static_cast<uint8_t>(t.tag)) && wr.put(t.year) && wr.put(t.month) && wr.put(t.day) &&
        wr.put(t.hour) && wr.put(t.minute) && wr.put(t.second) && wr.put(t.micro) &&
        wr.put(static_cast<uint8_t>(t.negative ? 1 : 0))))
    throw TooLarge{};
}

std::string time_text(const Temporal &t) {
  char b[48];
  if (t.micro == 0)
    std::snprintf(b, sizeof b, "%s%02u:%02u:%02u", t.negative ? "-" : "", t.hour, unsigned{t.minute},
                  unsigned{t.second});
  else
    std::snprintf(b, sizeof b, "%s%02u:%02u:%02u.%06u", t.negative ? "-" : "", t.hour, unsigned{t.minute},
                  unsigned{t.second}, t.micro);
  return b;
}

std::optional<Temporal> parse_time(std::string_view text) {
  const std::string s = trim(text);
  size_t i = 0;
  Temporal t{};
  t.tag = w::Tag::kTime;
  if (i < s.size() && s[i] == '-') t.negative = true, i++;
  const auto digits = [&](size_t min, size_t max, uint64_t *out) {
    size_t n = 0;
    uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && n < max + 1) v = v * 10 + (s[i++] - '0'), n++;
    *out = v;
    return n >= min && n <= max;
  };
  uint64_t h, m, sec, frac = 0;
  // Java's pattern (Polyglot.TIME): hours 1..4 digits, then :mm:ss[.f{1,6}]; minutes and seconds
  // not above 59.
  if (!digits(1, 4, &h) || i >= s.size() || s[i++] != ':' || !digits(2, 2, &m) || i >= s.size() ||
      s[i++] != ':' || !digits(2, 2, &sec) || m > 59 || sec > 59)
    return std::nullopt;
  if (i < s.size()) {
    if (s[i++] != '.') return std::nullopt;
    const size_t start = i;
    if (!digits(1, 6, &frac)) return std::nullopt;
    for (size_t n = i - start; n < 6; n++) frac *= 10;
  }
  if (i != s.size()) return std::nullopt;
  t.hour = static_cast<uint32_t>(h);
  t.minute = static_cast<uint8_t>(m);
  t.second = static_cast<uint8_t>(sec);
  t.micro = static_cast<uint32_t>(frac);
  return t;
}

std::optional<std::string> decimal_text(std::string_view s, std::string_view target) {
  std::optional<Decimal> d = Decimal::parse(trim(s));
  if (!d) return std::nullopt;
  if (d->scale < 0 || d->scale > static_cast<int64_t>(kDecimalMaxScale) ||
      d->precision() > static_cast<int64_t>(kDecimalMaxDigits)) {
    const int64_t integer_digits = d->precision() - d->scale;
    if (d->zero() || integer_digits < -static_cast<int64_t>(kDecimalMaxScale)) return std::string("0");
    const std::string what = target == "SQL" ? "a DECIMAL parameter" : std::string(target);
    if (integer_digits > static_cast<int64_t>(kDecimalMaxDigits))
      throw ConversionError{"value " + (integer_digits <= 80 ? d->plain()  // not 1e999999999's zeros
                                                             : "of " + std::to_string(integer_digits) + " digits") +
                            " is out of range for " + what};
    d->round_to(kDecimalMaxScale);
    if (d->precision() - d->scale > static_cast<int64_t>(kDecimalMaxDigits))
      throw ConversionError{"value is out of range for " + what};
  }
  return d->plain();
}

// ------------------------------------------------------------ to JavaScript

std::string Values::str(Local<Value> v) {
  v8::TryCatch tc(m_iso);
  Local<v8::String> s;
  if (v->IsSymbol()) return utf8(m_iso, v.As<v8::Symbol>()->Description(m_iso));
  if (!v->ToString(context()).ToLocal(&s)) return "?";
  return utf8(m_iso, s);
}

Local<Value> Values::integer(int64_t v, bool is_unsigned, const ParamType *t) {
  const bool big = is_unsigned && v < 0;  // above BIGINT's range
  const auto as_bigint = [&]() -> Local<Value> {
    return big ? Local<Value>(v8::BigInt::NewFromUnsigned(m_iso, static_cast<uint64_t>(v)))
               : Local<Value>(v8::BigInt::New(m_iso, v));
  };
  const auto as_string = [&]() -> Local<Value> {
    const std::string s = big ? std::to_string(static_cast<uint64_t>(v)) : std::to_string(v);
    return v8_string(m_iso, s);
  };
  const bool is_safe = !big && safe(v);
  if (t != nullptr && t->is("YEAR")) return v8::Number::New(m_iso, static_cast<double>(v));
  if (t != nullptr && t->is("BIT"))
    return is_safe && v >= 0 ? Local<Value>(v8::Number::New(m_iso, static_cast<double>(v)))
                             : Local<Value>(v8::BigInt::NewFromUnsigned(m_iso, static_cast<uint64_t>(v)));
  switch (rules()->integer_type) {
    case IntegerType::kString:
      return as_string();
    case IntegerType::kBigint:
      return as_bigint();
    case IntegerType::kUnsafeBigint:
      return is_safe ? Local<Value>(v8::Number::New(m_iso, static_cast<double>(v))) : as_bigint();
    case IntegerType::kUnsafeString:
    default:
      return is_safe ? Local<Value>(v8::Number::New(m_iso, static_cast<double>(v))) : as_string();
  }
}

Local<Value> Values::temporal(const Temporal &t) {
  if (t.tag == w::Tag::kTime) return v8_string(m_iso, time_text(t));
  using namespace std::chrono;
  const year_month_day ymd{year(t.year), month(t.month), day(t.day)};
  // Oracle's rule: a zero date (and MySQL's partial dates and impossible days, which no Date can
  // be) is an Invalid Date, as an argument and in a row. Year 0 with a month and a day is a date.
  // So is a clock time no DATETIME has: the host sends none.
  if (!ymd.ok() ||
      (t.tag == w::Tag::kDatetime && (t.hour > 23 || t.minute > 59 || t.second > 59 || t.micro > 999999)))
    return v8::Date::New(context(), std::nan("")).ToLocalChecked();
  const double ms = t.tag == w::Tag::kDate ? m_opt->epoch_ms(t.year, t.month, t.day, 0, 0, 0, 0)
                                           : m_opt->epoch_ms(t.year, t.month, t.day, t.hour, t.minute,
                                                             t.second, t.micro);
  return v8::Date::New(context(), ms).ToLocalChecked();
}

namespace {

Local<v8::ArrayBuffer> buffer_of(v8::Isolate *iso, std::span<const std::byte> data) {
  std::unique_ptr<v8::BackingStore> store = v8::ArrayBuffer::NewBackingStore(iso, data.size());
  if (!data.empty()) std::memcpy(store->Data(), data.data(), data.size());
  return v8::ArrayBuffer::New(iso, std::move(store));
}

/** A VECTOR as a Float32Array with a copy of the elements: guest code may keep it, and a view of
    the arena would be valid only until the worker's next write there (§7.2). */
Local<Value> vector_of(v8::Isolate *iso, Reader &r) {
  const auto dims = r.get<uint32_t>();
  r.align();
  const auto data = r.take(size_t{dims} * 4);
  return v8::Float32Array::New(buffer_of(iso, data), 0, dims);
}

}  // namespace

Local<Value> Values::argument(Reader &r, const ParamType &t) { return value(r, t, true); }

Local<Value> Values::column(Reader &r, const ParamType &t) { return value(r, t, false); }

Local<Value> Values::value(Reader &r, const ParamType &t, bool argument) {
  const auto tag = r.get<uint8_t>();
  // The value is decoded by its own tag, which is its type's unless it is NULL (§1, §7.3).
  if (tag != static_cast<uint8_t>(t.tag) && tag != static_cast<uint8_t>(w::Tag::kNull))
    protocol_failure("a value whose tag is not its type's");
  switch (static_cast<w::Tag>(tag)) {
    case w::Tag::kNull:
      return v8::Null(m_iso);
    case w::Tag::kI64:
      return integer(r.get<int64_t>(), false, &t);
    case w::Tag::kU64:
      return integer(static_cast<int64_t>(r.get<uint64_t>()), true, &t);
    case w::Tag::kF64:  // any bit pattern (§7.2)
      return v8::Number::New(m_iso, r.get<double>());
    case w::Tag::kF32:
      return v8::Number::New(m_iso, r.get<float>());
    case w::Tag::kDecimal: {
      const std::string_view s = r.str();
      if (rules()->decimal_type == DecimalType::kNumber)
        return v8::Number::New(m_iso, std::strtod(std::string(s).c_str(), nullptr));
      return v8_string(m_iso, s);
    }
    case w::Tag::kString: {
      // An lstr: ill-formed UTF-8 of the server's data is repaired by v8_string (§7.1).
      const std::string_view s = r.str();
      if (argument && t.is("SET")) {  // in a row a SET stays a string (Oracle's SQL API)
        const Local<v8::Set> set = v8::Set::New(m_iso);
        if (!s.empty()) {
          size_t start = 0;
          while (true) {
            const size_t comma = s.find(',', start);
            const std::string_view item = s.substr(start, comma == std::string_view::npos ? s.npos : comma - start);
            if (set->Add(context(), v8_string(m_iso, item)).IsEmpty() || comma == std::string_view::npos) break;
            start = comma + 1;
          }
        }
        return set;
      }
      return v8_string(m_iso, s);
    }
    case w::Tag::kBytes: {
      const auto n = r.get<uint32_t>();
      const auto data = r.take(n);
      return v8::Uint8Array::New(buffer_of(m_iso, data), 0, n);
    }
    case w::Tag::kDate:
    case w::Tag::kTime:
    case w::Tag::kDatetime:
      return temporal(read_temporal(r, static_cast<w::Tag>(tag)));
    case w::Tag::kTimestamp: {
      // An instant: a Date, whatever the session's zone. Date counts milliseconds. MySQL's zero
      // timestamp is an Invalid Date (Oracle's rule).
      const auto us = r.get<int64_t>();
      if (us == w::kZeroTimestamp) return v8::Date::New(context(), std::nan("")).ToLocalChecked();
      return v8::Date::New(context(), static_cast<double>(us / 1000)).ToLocalChecked();
    }
    case w::Tag::kJson: {
      const v8::TryCatch tc(m_iso);
      Local<Value> parsed;
      if (!v8::JSON::Parse(context(), v8_string(m_iso, r.str())).ToLocal(&parsed))
        throw ConversionError{"invalid JSON from MySQL"};
      return parsed;
    }
    case w::Tag::kVector:
      return vector_of(m_iso, r);
  }
  protocol_failure("an unknown value tag");
}

// ------------------------------------------------------------ from JavaScript

void Values::cannot(Local<Value> v, const ParamType &t) {
  const std::string shown = v->IsString() ? utf8(m_iso, v) : str(v);
  throw ConversionError{"Cannot convert value '" + shown + "' to " + t.sql_name() + " from MLE"};
}

namespace {

/** {min, max} of a SQL integer type as signed 128-bit numbers. */
std::pair<__int128, __int128> int_range(const ParamType &t) {
  int bits = 64;
  if (t.type_name == "TINYINT") bits = 8;
  else if (t.type_name == "SMALLINT") bits = 16;
  else if (t.type_name == "MEDIUMINT") bits = 24;
  else if (t.type_name == "INT") bits = 32;
  if (t.is_unsigned || t.tag == w::Tag::kU64) return {0, (static_cast<__int128>(1) << bits) - 1};
  return {-(static_cast<__int128>(1) << (bits - 1)), (static_cast<__int128>(1) << (bits - 1)) - 1};
}

bool put_tag(Writer &wr, w::Tag tag) { return wr.put(static_cast<uint8_t>(tag)); }

void need(bool ok) {
  if (!ok) throw TooLarge{};
}

/** A whole decimal string (sign, digits) as a 128-bit integer; false if it does not fit. */
bool parse_int128(const std::string &plain, __int128 *out) {
  __int128 v = 0;
  size_t i = 0;
  const bool neg = !plain.empty() && plain[0] == '-';
  if (neg) i = 1;
  if (i == plain.size()) return false;
  for (; i < plain.size(); i++) {
    if (plain[i] < '0' || plain[i] > '9') return false;
    if (v > (static_cast<__int128>(1) << 100)) return false;
    v = v * 10 + (plain[i] - '0');
  }
  *out = neg ? -v : v;
  return true;
}

}  // namespace

void Values::write_as(Writer &wr, Local<Value> v, const ParamType &t) {
  if (v.IsEmpty() || v->IsNullOrUndefined()) {
    need(put_tag(wr, w::Tag::kNull));
    return;
  }
  const Local<v8::Context> ctx = context();
  const bool set_type = t.is("SET") || t.is("ENUM");
  if (set_type) {
    std::string text;
    if (v->IsSet()) {
      const Local<v8::Array> items = v.As<v8::Set>()->AsArray();
      for (uint32_t i = 0; i < items->Length(); i++) {
        if (i > 0) text += ',';
        text += str(items->Get(ctx, i).ToLocalChecked());
      }
    } else {
      text = str(v);
    }
    need(put_tag(wr, w::Tag::kString) && wr.str(text));
    return;
  }
  switch (t.tag) {
    case w::Tag::kI64:
    case w::Tag::kU64: {
      __int128 b = 0;
      if (v->IsBoolean()) {
        b = v->BooleanValue(m_iso) ? 1 : 0;
      } else if (v->IsBigInt()) {
        bool lossless = false;
        const int64_t s = v.As<v8::BigInt>()->Int64Value(&lossless);
        if (lossless) {
          b = s;
        } else {
          const uint64_t u = v.As<v8::BigInt>()->Uint64Value(&lossless);
          if (!lossless) cannot(v, t);
          b = u;
        }
      } else if (v->IsNumber()) {
        const double d = v.As<v8::Number>()->Value();
        if (!std::isfinite(d)) cannot(v, t);
        const double r = d == std::floor(d) ? d : std::floor(d + 0.5);  // Math.round
        if (std::fabs(r) > 1.8e19) cannot(v, t);
        b = static_cast<__int128>(r);
      } else if (v->IsString()) {
        const std::optional<Decimal> d = Decimal::parse(trim(utf8(m_iso, v)));
        if (!d) cannot(v, t);
        Decimal rounded = *d;
        rounded.round_to(0);
        // At most 40 digits before the text ("1e999999999" would be a billion zeros).
        if (rounded.precision() - rounded.scale > 40 || !parse_int128(rounded.plain(), &b)) cannot(v, t);
      } else {
        cannot(v, t);
      }
      const auto [lo, hi] = int_range(t);
      if (b < lo || b > hi) cannot(v, t);
      // The tag of the declared type (§7.3: an unsigned type below BIGINT is I64).
      const bool as_u64 = t.tag == w::Tag::kU64 || b > INT64_MAX;
      need(put_tag(wr, as_u64 ? w::Tag::kU64 : w::Tag::kI64) &&
           wr.put(static_cast<uint64_t>(static_cast<unsigned __int128>(b))));
      return;
    }
    case w::Tag::kF64:
    case w::Tag::kF32: {
      double d;
      if (v->IsBoolean()) {
        d = v->BooleanValue(m_iso) ? 1 : 0;
      } else if (v->IsNumber()) {
        d = v.As<v8::Number>()->Value();
      } else if (v->IsString()) {
        const std::string s = trim(utf8(m_iso, v));
        // Java's Double.parseDouble: decimal notation only (no hex, no Infinity as a number here).
        if (s.empty() || s.find_first_of("xXnN") != std::string::npos || s.find("Inf") != std::string::npos)
          cannot(v, t);
        char *end = nullptr;
        errno = 0;
        d = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size()) cannot(v, t);
      } else {
        cannot(v, t);
      }
      if (!std::isfinite(d)) cannot(v, t);
      if (t.tag == w::Tag::kF32)
        need(put_tag(wr, w::Tag::kF32) && wr.put(static_cast<float>(d)));
      else
        need(put_tag(wr, w::Tag::kF64) && wr.put(d));
      return;
    }
    case w::Tag::kDecimal: {
      std::string text;
      if (v->IsBoolean()) {
        text = v->BooleanValue(m_iso) ? "1" : "0";
      } else if (v->IsBigInt() || v->IsNumber() || v->IsString()) {
        if (v->IsNumber() && !std::isfinite(v.As<v8::Number>()->Value())) cannot(v, t);
        text = v->IsString() ? trim(utf8(m_iso, v)) : str(v);
      } else {
        cannot(v, t);
      }
      std::optional<std::string> d = decimal_text(text, t.sql_name());
      if (!d || !valid_decimal(*d)) cannot(v, t);
      need(put_tag(wr, w::Tag::kDecimal) && wr.str(*d));
      return;
    }
    case w::Tag::kString: {
      const std::string s = v->IsString() ? utf8(m_iso, v) : str(v);
      need(put_tag(wr, w::Tag::kString) && wr.str(s));
      return;
    }
    case w::Tag::kJson: {
      // Oracle: a string is JSON text (checked); an object or array is stringified; a bare number,
      // boolean or BigInt is not a JSON document.
      if (v->IsBoolean() || v->IsNumber() || v->IsBigInt()) cannot(v, t);
      v8::TryCatch tc(m_iso);
      Local<Value> obj = v;
      if (v->IsString() && !v8::JSON::Parse(ctx, v.As<v8::String>()).ToLocal(&obj)) cannot(v, t);
      Local<v8::String> text;
      if (!v8::JSON::Stringify(ctx, obj).ToLocal(&text) || text.IsEmpty() || tc.HasCaught()) cannot(v, t);
      Local<Value> as_value = text;
      if (!as_value->IsString()) cannot(v, t);
      need(put_tag(wr, w::Tag::kJson) && wr.str(utf8(m_iso, text)));
      return;
    }
    case w::Tag::kBytes: {
      std::span<const std::byte> data;
      std::shared_ptr<v8::BackingStore> keep;
      if (v->IsArrayBuffer()) {
        keep = v.As<v8::ArrayBuffer>()->GetBackingStore();
        data = {static_cast<const std::byte *>(keep->Data()), keep->ByteLength()};
      } else if (v->IsArrayBufferView()) {
        const Local<v8::ArrayBufferView> view = v.As<v8::ArrayBufferView>();
        keep = view->Buffer()->GetBackingStore();
        data = {static_cast<const std::byte *>(keep->Data()) + view->ByteOffset(), view->ByteLength()};
      } else {
        cannot(v, t);
      }
      need(put_tag(wr, w::Tag::kBytes) && wr.put(static_cast<uint32_t>(data.size())) && wr.bytes(data));
      return;
    }
    case w::Tag::kVector: {
      std::vector<float> f;
      if (v->IsFloat32Array()) {
        const Local<v8::Float32Array> a = v.As<v8::Float32Array>();
        if (a->Length() > w::kMaxVectorDimensions) throw ConversionError{"VECTOR too large: " + std::to_string(a->Length())};
        f.resize(a->Length());
        a->CopyContents(f.data(), f.size() * 4);
      } else if (v->IsArray() || v->IsArrayBufferView()) {
        // Float32Array.from(v)
        const Local<v8::Object> o = v.As<v8::Object>();
        Local<Value> len;
        if (!o->Get(ctx, v8_string(m_iso, "length")).ToLocal(&len) || !len->IsNumber()) cannot(v, t);
        const double n = len.As<v8::Number>()->Value();
        if (!(n >= 0 && n == std::floor(n))) cannot(v, t);
        if (n > w::kMaxVectorDimensions) throw ConversionError{"VECTOR too large: " + std::to_string(int64_t(n))};
        f.resize(static_cast<size_t>(n));
        for (uint32_t i = 0; i < f.size(); i++) {
          Local<Value> e;
          double x = std::nan("");
          if (o->Get(ctx, i).ToLocal(&e)) {
            Local<v8::Number> num;
            if (e->ToNumber(ctx).ToLocal(&num)) x = num->Value();
          }
          f[i] = static_cast<float>(x);  // the host checks that the elements are finite (§7.2)
        }
      } else {
        cannot(v, t);
      }
      if (f.size() > w::kMaxVectorDimensions) throw ConversionError{"VECTOR too large: " + std::to_string(f.size())};
      if (f.empty()) throw ConversionError{"a VECTOR needs at least one dimension"};
      need(put_tag(wr, w::Tag::kVector) && wr.put(static_cast<uint32_t>(f.size())) && wr.align() &&
           wr.bytes(std::as_bytes(std::span<const float>(f))));
      return;
    }
    case w::Tag::kTimestamp: {
      // The instant itself (§7.2), within the wire's range.
      if (!v->IsDate()) cannot(v, t);
      const double ms = v.As<v8::Date>()->ValueOf();
      if (!std::isfinite(ms) || ms < 0 || ms > static_cast<double>(w::kMaxTimestamp / 1000)) cannot(v, t);
      need(put_tag(wr, w::Tag::kTimestamp) && wr.put(static_cast<int64_t>(ms) * 1000));
      return;
    }
    case w::Tag::kDate:
    case w::Tag::kDatetime: {
      if (!v->IsDate()) cannot(v, t);
      const double ms = v.As<v8::Date>()->ValueOf();
      if (!std::isfinite(ms)) cannot(v, t);
      int year;
      unsigned month, day, hour, minute, second, micro;
      m_opt->local(ms, &year, &month, &day, &hour, &minute, &second, &micro);
      if (year < 0 || year > 9999) cannot(v, t);
      Temporal out{};
      out.tag = t.tag;
      out.year = static_cast<uint16_t>(year);
      out.month = static_cast<uint8_t>(month);
      out.day = static_cast<uint8_t>(day);
      if (t.tag != w::Tag::kDate) {
        out.hour = hour;
        out.minute = static_cast<uint8_t>(minute);
        out.second = static_cast<uint8_t>(second);
        out.micro = micro;
      }
      if (!valid_temporal(out)) cannot(v, t);
      write_temporal(wr, out);
      return;
    }
    case w::Tag::kTime: {
      const std::string s = v->IsString() ? utf8(m_iso, v) : str(v);
      std::optional<Temporal> tm = parse_time(s);
      if (!tm) throw ConversionError{"Cannot convert value '" + s + "' to TIME from MLE"};
      if (tm->hour > w::kMaxTimeHour || (tm->hour == w::kMaxTimeHour && tm->minute == 59 && tm->second == 59 &&
                                          tm->micro > 0))
        throw ConversionError{"TIME value out of range: " + s};
      if (!valid_temporal(*tm)) cannot(v, t);
      write_temporal(wr, *tm);
      return;
    }
    case w::Tag::kNull:
      break;
  }
  cannot(v, t);
}

void Values::write_param(Writer &wr, Local<Value> v) {
  const Local<v8::Context> ctx = context();
  if (v.IsEmpty() || v->IsNullOrUndefined()) {
    need(put_tag(wr, w::Tag::kNull));
  } else if (v->IsBoolean()) {
    need(put_tag(wr, w::Tag::kI64) && wr.put<int64_t>(v->BooleanValue(m_iso) ? 1 : 0));
  } else if (v->IsString()) {
    need(put_tag(wr, w::Tag::kString) && wr.str(utf8(m_iso, v)));
  } else if (v->IsNumber()) {
    const double d = v.As<v8::Number>()->Value();
    if (!std::isfinite(d)) throw ConversionError{"cannot bind " + str(v)};
    if (d == std::floor(d) && std::fabs(d) <= static_cast<double>(kMaxSafe))
      need(put_tag(wr, w::Tag::kI64) && wr.put(static_cast<int64_t>(d)));
    else
      need(put_tag(wr, w::Tag::kF64) && wr.put(d));
  } else if (v->IsBigInt()) {
    bool lossless = false;
    const int64_t s = v.As<v8::BigInt>()->Int64Value(&lossless);
    if (lossless) {
      need(put_tag(wr, w::Tag::kI64) && wr.put(s));
    } else {
      const uint64_t u = v.As<v8::BigInt>()->Uint64Value(&lossless);
      if (lossless) {
        need(put_tag(wr, w::Tag::kU64) && wr.put(u));
      } else {
        const std::optional<std::string> d = decimal_text(str(v), "SQL");
        if (!d || !valid_decimal(*d)) throw ConversionError{"cannot bind " + str(v)};
        need(put_tag(wr, w::Tag::kDecimal) && wr.str(*d));
      }
    }
  } else if (v->IsDate()) {
    const double ms = v.As<v8::Date>()->ValueOf();
    if (!std::isfinite(ms)) throw ConversionError{"cannot bind an invalid Date"};
    ParamType t;
    t.tag = w::Tag::kDatetime;
    write_as(wr, v, t);
  } else if (v->IsFloat32Array()) {
    ParamType t;
    t.tag = w::Tag::kVector;
    write_as(wr, v, t);
  } else if (v->IsArrayBuffer() || v->IsArrayBufferView()) {
    ParamType t;
    t.tag = w::Tag::kBytes;
    write_as(wr, v, t);
  } else if (v->IsSet()) {
    ParamType t;
    t.tag = w::Tag::kString;
    t.type_name = "SET";
    write_as(wr, v, t);
  } else if (v->IsSymbol() || v->IsFunction()) {
    throw ConversionError{std::string("cannot bind a ") + (v->IsSymbol() ? "symbol" : "function")};
  } else {
    v8::TryCatch tc(m_iso);
    Local<v8::String> json;
    if (!v8::JSON::Stringify(ctx, v).ToLocal(&json) || tc.HasCaught() || !Local<Value>(json)->IsString())
      throw ConversionError{"cannot bind " + str(v)};
    need(put_tag(wr, w::Tag::kJson) && wr.str(utf8(m_iso, json)));
  }
}

}  // namespace mle::v8w
