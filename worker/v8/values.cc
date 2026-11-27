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
#include <limits>
#include <regex>
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

/** ECMAScript's StrWhiteSpaceChar (what Number() strips): white space, the Zs category and line
    terminators. */
bool js_space(char32_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
         c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/** The code point that starts at s[i] (well-formed UTF-8), and its length. */
char32_t code_point(std::string_view s, size_t i, size_t *len) {
  const auto b = static_cast<unsigned char>(s[i]);
  const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
  if (i + n > s.size()) {
    *len = 1;
    return b;
  }
  char32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
  for (size_t k = 1; k < n; k++) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  *len = n;
  return c;
}

/** s without the white space Number() ignores at either end. */
std::string trim(std::string_view s) {
  size_t len = 0;
  while (!s.empty() && js_space(code_point(s, 0, &len))) s.remove_prefix(len);
  while (!s.empty()) {
    size_t start = s.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
    if (!js_space(code_point(s, start, &len))) break;
    s.remove_suffix(s.size() - start);
  }
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
        if (s[i] < '0' || s[i] > '9' || exp > 100000000000) return std::nullopt;
        exp = exp * 10 + (s[i] - '0');
      }
      if (eneg) exp = -exp;
    }
    if (i != s.size()) return std::nullopt;
    const size_t nz = all.find_first_not_of('0');
    d.digits = nz == std::string::npos ? "0" : all.substr(nz);
    d.scale = frac - exp;
    // Java's scale is an int: a number whose scale is not one is not a number there either.
    if (d.scale < INT32_MIN || d.scale > INT32_MAX) return std::nullopt;
    if (d.zero()) d.negative = false;
    return d;
  }

  /** Rounds to at most `to` digits after the point: a half away from zero (HALF_UP), or, with
      `math_round`, towards +Infinity as JavaScript's Math.round does. */
  void round_to(int64_t to, bool math_round = false) {
    if (scale <= to) return;
    const int64_t drop = scale - to;
    if (drop > precision()) {
      digits = "0";
      negative = false;
      scale = to;
      return;
    }
    const size_t first = digits.size() - static_cast<size_t>(drop);
    bool up = digits[first] >= '5';
    if (math_round && negative && digits[first] == '5')
      up = digits.find_first_not_of('0', first + 1) != std::string::npos;  // more than a half
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

  /** Java's BigDecimal.toString(): plain, or with an exponent ("1E+101") when the scale is negative
      or the number is below 1e-6 (the GraalJS worker's messages show numbers so). */
  std::string java() const {
    if (scale == 0) return (negative ? "-" : "") + digits;
    const int64_t adjusted = precision() - 1 - scale;
    if (scale > 0 && adjusted >= -6) return plain();
    std::string out = (negative ? "-" : "") + digits.substr(0, 1);
    if (digits.size() > 1) out += "." + digits.substr(1);
    if (adjusted != 0) out += "E" + std::string(adjusted > 0 ? "+" : "") + std::to_string(adjusted);
    return out;
  }
};

/** Year, month and day of days since 1970-01-01 in the proleptic Gregorian calendar, for every
    day a Date has (std::chrono's year ends at 32767). */
void civil_from_days(int64_t z, int64_t *year, unsigned *month, unsigned *day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
  *year = static_cast<int64_t>(yoe) + era * 400 + (*month <= 2 ? 1 : 0);
}

/** Days since 1970-01-01 of a date of the proleptic Gregorian calendar (month 1 to 12). */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/** Seconds since the epoch within the years a zone is asked about: tzdb's get_info throws beyond
    std::chrono's years, and a Date reaches 275760. Outside 1000 BC to AD 11000, where no MySQL
    date is, a time takes the offset at that edge. */
constexpr std::chrono::seconds kZoneLow =
    std::chrono::sys_days{std::chrono::year{-1000} / std::chrono::January / 1}.time_since_epoch();
constexpr std::chrono::seconds kZoneHigh =
    std::chrono::sys_days{std::chrono::year{11000} / std::chrono::January / 1}.time_since_epoch();

std::chrono::seconds zone_range(std::chrono::seconds s) { return std::clamp(s, kZoneLow, kZoneHigh); }

/** Java's Instant.toString() of milliseconds since the epoch ("1969-12-31T23:59:59.999Z",
    "+10000-01-01T00:00:00Z"), as the GraalJS worker's TIMESTAMP messages show an instant. */
std::string instant_text(int64_t ms) {
  const int64_t secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
  const auto milli = static_cast<unsigned>(ms - secs * 1000);
  const int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
  const auto rest = static_cast<unsigned>(secs - days * 86400);
  int64_t year;
  unsigned month, day;
  civil_from_days(days, &year, &month, &day);
  char y[32];
  if (year < 0)
    std::snprintf(y, sizeof y, "-%04lld", static_cast<long long>(-year));
  else
    std::snprintf(y, sizeof y, year > 9999 ? "+%lld" : "%04lld", static_cast<long long>(year));
  char b[80];
  std::snprintf(b, sizeof b, "%s-%02u-%02uT%02u:%02u:%02u", y, month, day, rest / 3600, rest / 60 % 60, rest % 60);
  std::string out = b;
  if (milli != 0) {
    std::snprintf(b, sizeof b, ".%03u", milli);
    out += b;
  }
  return out + "Z";
}

/** Java's Duration.toString() of a TIME ("PT839H", "PT-839H-30M-0.5S"), as the GraalJS worker's
    TIME messages show one. */
std::string duration_text(const Temporal &t) {
  const int64_t total = (int64_t{t.hour} * 60 + t.minute) * 60 + t.second;
  const int64_t nanos_abs = int64_t{t.micro} * 1000;
  if (total == 0 && nanos_abs == 0) return "PT0S";
  // Duration keeps floor seconds and nanoseconds 0 to 999999999.
  int64_t seconds = t.negative ? -total : total;
  int64_t nanos = nanos_abs;
  if (t.negative && nanos_abs > 0) {
    seconds--;
    nanos = 1000000000 - nanos_abs;
  }
  const int64_t effective = seconds < 0 && nanos > 0 ? seconds + 1 : seconds;
  const int64_t hours = effective / 3600, minutes = effective % 3600 / 60, secs = effective % 60;
  std::string out = "PT";
  if (hours != 0) out += std::to_string(hours) + "H";
  if (minutes != 0) out += std::to_string(minutes) + "M";
  if (secs == 0 && nanos == 0 && out.size() > 2) return out;
  if (seconds < 0 && nanos > 0)
    out += secs == 0 ? "-0" : std::to_string(secs);
  else
    out += std::to_string(secs);
  if (nanos > 0) {
    std::string frac = std::to_string(seconds < 0 ? 2 * 1000000000 - nanos : nanos + 1000000000);
    while (frac.back() == '0') frac.pop_back();
    frac[0] = '.';
    out += frac;
  }
  return out + "S";
}

}  // namespace

// ------------------------------------------------------------ strings

namespace {

/** The most UTF-8 bytes a string of n UTF-16 units can need: 2 per unit of a one-byte string, 3
    per unit otherwise (a pair is 4 bytes for 2 units; an unpaired surrogate becomes U+FFFD, 3). */
size_t utf8_bound(Local<v8::String> s) { return size_t(s->Length()) * (s->IsOneByte() ? 2 : 3); }

/** Strings up to this bound are encoded into a buffer of the bound, in one pass; longer ones are
    measured first (two passes, but no buffer three times their size). */
constexpr size_t kOnePassBound = 16U << 20;

size_t write_utf8_into(v8::Isolate *iso, Local<v8::String> s, std::span<std::byte> to) {
  // With room for the bound, V8 encodes without checking the room per character; an unpaired
  // surrogate becomes U+FFFD (§7.1).
  return static_cast<size_t>(s->WriteUtf8(iso, reinterpret_cast<char *>(to.data()), static_cast<int>(to.size()),
                                          nullptr, v8::String::NO_NULL_TERMINATION | v8::String::REPLACE_INVALID_UTF8));
}

std::string utf8_of(v8::Isolate *iso, Local<v8::String> s) {
  const size_t bound = utf8_bound(s);
  std::string out(bound <= kOnePassBound ? bound : size_t(s->Utf8Length(iso)), '\0');
  out.resize(write_utf8_into(iso, s, std::as_writable_bytes(std::span(out.data(), out.size()))));
  return out;
}

}  // namespace

std::string utf8(v8::Isolate *iso, Local<Value> v) {
  if (v.IsEmpty()) return "";
  Local<v8::String> s;
  if (v->IsString()) {
    s = v.As<v8::String>();
  } else {
    const v8::TryCatch tc(iso);
    if (!v->ToString(iso->GetCurrentContext()).ToLocal(&s)) return "?";
  }
  return utf8_of(iso, s);
}

bool write_utf8(v8::Isolate *iso, Writer &w, Local<v8::String> s) {
  const auto fill = [&](std::span<std::byte> to) { return write_utf8_into(iso, s, to); };
  const size_t bound = utf8_bound(s);
  if (bound <= kOnePassBound && w.str_in_place(bound, fill)) return true;
  // Longer, or the bound does not fit what is left of the message: its exact length.
  return w.str_in_place(size_t(s->Utf8Length(iso)), fill);
}

std::optional<std::string> utf8_exact(v8::Isolate *iso, Local<v8::String> s) {
  std::string out = utf8_of(iso, s);
  // A one-byte string has no surrogates; another is looked at only if its encoding has a U+FFFD,
  // which a repair or the text itself put there.
  if (s->IsOneByte() || out.find("\xEF\xBF\xBD") == std::string::npos) return out;
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
  return out;
}

Local<v8::String> v8_string(v8::Isolate *iso, std::string_view s, bool key) {
  Local<v8::String> out;
  if (s.size() > static_cast<size_t>(v8::String::kMaxLength) ||
      !v8::String::NewFromUtf8(iso, s.data(),
                               key && s.size() < 32 ? v8::NewStringType::kInternalized : v8::NewStringType::kNormal,
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
  m_instants = m_locals = Interval{};
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
  // Fields past their ranges roll over, as the Date constructor's do (2024-04-31 is 1 May).
  const int64_t y = int64_t{year} + (month - 1) / 12;
  const local_days d{days(days_from_civil(y, (month - 1) % 12 + 1, 1) + (int64_t{day} - 1))};
  // In seconds: microseconds since the epoch of a host's hour field (2^32 hours) would overflow.
  const local_seconds t = d + hours(hour) + minutes(minute) + seconds(second);
  seconds off = offset;
  if (zone != nullptr) {
    const int64_t at = t.time_since_epoch().count();
    if (m_locals.holds(at)) {
      off = seconds(m_locals.offset);
    } else {
      // Java's atZone: a local time in a gap moves forward by the gap (the offset before it); in
      // an overlap the earlier offset.
      const local_info info = zone->get_info(local_seconds{zone_range(t.time_since_epoch())});
      off = info.first.offset;
      if (info.result == local_info::unique) {
        // The local times of this offset that no neighbouring offset repeats or skips: from the
        // transition at its start, shifted by the larger of the two offsets there, to the one
        // at its end, shifted by the smaller (Appendix A). Within the years zone_range asks.
        const sys_info &s = info.first;
        const seconds begin = std::max(s.begin.time_since_epoch(), kZoneLow);
        const seconds end = std::min(s.end.time_since_epoch(), kZoneHigh);
        const seconds before = begin > kZoneLow ? zone->get_info(sys_seconds{begin - seconds(1)}).offset : s.offset;
        const seconds after = end < kZoneHigh ? zone->get_info(sys_seconds{end}).offset : s.offset;
        m_locals = {(begin + std::max(s.offset, before)).count(), (end + std::min(s.offset, after)).count(),
                    s.offset.count()};
      }
    }
  }
  // Milliseconds as Java's toEpochMilli (floor), as a double for Date (beyond its range: NaN).
  return static_cast<double>((t.time_since_epoch() - off).count() * 1000 + micro / 1000);
}

void Options::local(double ms, int *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute,
                    unsigned *second, unsigned *micro) const {
  using namespace std::chrono;
  const sys_time<milliseconds> t{milliseconds(static_cast<int64_t>(std::floor(ms)))};
  seconds off = offset;
  if (zone != nullptr) {
    const seconds at = floor<seconds>(t).time_since_epoch();
    if (m_instants.holds(at.count())) {
      off = seconds(m_instants.offset);
    } else {
      const sys_info s = zone->get_info(sys_seconds{zone_range(at)});
      off = s.offset;
      // The offset holds from one transition to the next (Appendix A).
      m_instants = {s.begin.time_since_epoch().count(), s.end.time_since_epoch().count(), s.offset.count()};
    }
  }
  const auto local = t + off;
  const auto days = floor<std::chrono::days>(local);
  const auto rest = local - days;
  int64_t y;
  civil_from_days(days.time_since_epoch().count(), &y, month, day);
  *year = static_cast<int>(y);
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
  // A zero TIME has no sign (§7.2: "-00:00:00" is sent as 00:00:00, as Java's Duration has it).
  if (h == 0 && m == 0 && sec == 0 && frac == 0) t.negative = false;
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
    // The number as Java's BigDecimal shows it ("1E+101"), as the GraalJS worker's message does.
    if (integer_digits > static_cast<int64_t>(kDecimalMaxDigits))
      throw ConversionError{"value " + (d->precision() <= 40 ? d->java()
                                                             : "of " + std::to_string(integer_digits) + " digits") +
                            " is out of range for " + what};
    d->round_to(kDecimalMaxScale);
    if (d->precision() - d->scale > static_cast<int64_t>(kDecimalMaxDigits))
      throw ConversionError{"value is out of range for " + what};
  }
  return d->plain();
}

// ------------------------------------------------------------ to JavaScript

namespace {

/** String(symbol): "Symbol(description)" (SymbolDescriptiveString). */
std::string symbol_text(v8::Isolate *iso, Local<v8::Symbol> s) {
  const Local<Value> d = s->Description(iso);
  return "Symbol(" + (d->IsString() ? utf8(iso, d) : std::string()) + ")";
}

}  // namespace

std::string Values::str(Local<Value> v) {
  const v8::TryCatch tc(m_iso);
  if (v->IsString()) return utf8(m_iso, v);
  if (v->IsSymbol()) return symbol_text(m_iso, v.As<v8::Symbol>());
  Local<v8::String> s;
  if (!v->ToString(context()).ToLocal(&s)) return "?";
  return utf8(m_iso, s);
}

Local<v8::String> Values::string_of(Local<Value> v) {
  if (v->IsString()) return v.As<v8::String>();
  if (v->IsSymbol()) return v8_string(m_iso, symbol_text(m_iso, v.As<v8::Symbol>()));
  // ToString runs the object's toString or valueOf: what that throws stays pending.
  Local<v8::String> s;
  if (!v->ToString(context()).ToLocal(&s)) throw GuestThrew{};
  return s;
}

std::string Values::text(Local<Value> v) { return utf8(m_iso, string_of(v)); }

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
  // Oracle's rule (srjs-data-arguments): a zero date, or one with a zero month or day, is an
  // Invalid Date, as an argument and in a row; an invalid date such as 31 February is given to the
  // Date constructor with its fields, which roll over (2024-02-31 is 2 March). Year 0 is a year.
  if (t.month == 0 || t.day == 0) return v8::Date::New(context(), std::nan("")).ToLocalChecked();
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
  // The value is decoded and converted by its own tag, which is not compared with its type's
  // (§1 item 2): another tag gives guest code a value of that tag's type.
  const auto tag = r.get<uint8_t>();
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
      return v8_string(m_iso, s, false);
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
      return v8_string(m_iso, s, false);
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
  throw ConversionError{"Cannot convert value '" + str(v) + "' to " + t.sql_name() + " from MLE"};
}

namespace {

/** After a failure under tc: a termination (an interrupt, the heap limit) goes on to the caller's
    TryCatch, as GuestThrew. */
void keep_termination(v8::TryCatch &tc) {
  if (tc.HasTerminated()) {
    tc.ReThrow();
    throw GuestThrew{};
  }
}

}  // namespace

std::optional<std::string> Values::json_text(Local<Value> v) {
  const Local<v8::Context> ctx = context();
  // JSON text is parsed and written again; text that is not JSON (or whose objects give none,
  // by an Object.prototype.toJSON) has none.
  const auto of_text = [&](Local<v8::String> s) -> std::optional<std::string> {
    v8::TryCatch tc(m_iso);
    Local<Value> parsed;
    Local<v8::String> out;
    if (!v8::JSON::Parse(ctx, s).ToLocal(&parsed) || !v8::JSON::Stringify(ctx, parsed).ToLocal(&out)) {
      keep_termination(tc);
      return std::nullopt;
    }
    std::string text = utf8(m_iso, out);
    if (text == "undefined") return std::nullopt;
    return text;
  };
  if (v->IsString()) return of_text(v.As<v8::String>());
  // A scalar that is not a string (typeof is not 'object') is not a JSON document (Oracle).
  if (!v->IsObject() || v->IsFunction()) return std::nullopt;
  // An object with a toJSON method is what that gives: a string is JSON text (a Date's is not
  // JSON), anything else is stringified. Other objects are stringified. What guest code throws
  // (toJSON, a getter, a BigInt in a container) stays pending.
  const Local<v8::Object> o = v.As<v8::Object>();
  Local<Value> to_json;
  if (!o->Get(ctx, v8_string(m_iso, "toJSON")).ToLocal(&to_json)) throw GuestThrew{};
  Local<Value> what = v;
  if (to_json->IsFunction()) {
    Local<Value> key = v8::String::Empty(m_iso);
    if (!to_json.As<v8::Function>()->Call(ctx, o, 1, &key).ToLocal(&what)) throw GuestThrew{};
    if (what->IsString()) return of_text(what.As<v8::String>());
  }
  Local<v8::String> out;
  if (!v8::JSON::Stringify(ctx, what).ToLocal(&out)) throw GuestThrew{};
  // V8's Stringify gives String(undefined) where JSON.stringify gives undefined (a symbol, a
  // function, a toJSON that returns nothing): that has no JSON. No JSON text is that word.
  std::string text = utf8(m_iso, out);
  if (text == "undefined") return std::nullopt;
  return text;
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

Values::Values(v8::Isolate *isolate, Local<v8::Context> context, const Options *options)
    : m_iso(isolate), m_ctx(isolate, context), m_opt(options) {
  Local<Value> d;
  if (context->Global()->Get(context, v8_string(isolate, "Date")).ToLocal(&d) && d->IsFunction())
    m_date.Reset(isolate, d.As<v8::Function>());
}

double Values::date_ms(Local<Value> v) {
  if (v->IsDate()) return v.As<v8::Date>()->ValueOf();
  if (!v->IsString() || m_date.IsEmpty()) return std::numeric_limits<double>::quiet_NaN();
  // MySQL's literal forms are local time in the zone of OPTIONS: an ECMAScript date-time without
  // an offset is local, a date-only one would be UTC (a DATE west of UTC: the day before).
  static const std::regex literal(R"(^(\d{4}-\d{2}-\d{2})(?:[ T](\d{2}:\d{2})(?::(\d{2})(?:\.(\d{1,9}))?)?)?$)");
  const std::string s = trim(utf8(m_iso, v));
  std::smatch m;
  Local<Value> arg = v;
  if (std::regex_match(s, m, literal)) {
    std::string frac = m[4].str();
    frac.resize(3, '0');
    arg = v8_string(m_iso, m[1].str() + 'T' + (m[2].matched ? m[2].str() : "00:00") + ':' +
                               (m[3].matched ? m[3].str() : "00") + '.' + frac);
  }
  const Local<v8::Context> ctx = context();
  Local<v8::Object> date;
  if (!m_date.Get(m_iso)->NewInstance(ctx, 1, &arg).ToLocal(&date)) throw GuestThrew{};
  return date->IsDate() ? date.As<v8::Date>()->ValueOf() : std::numeric_limits<double>::quiet_NaN();
}

std::optional<double> Values::year_of(Local<Value> v) {
  if (v->IsDate()) {
    const double ms = v.As<v8::Date>()->ValueOf();
    if (!std::isfinite(ms)) return std::numeric_limits<double>::quiet_NaN();
    int year;
    unsigned month, day, hour, minute, second, micro;
    m_opt->local(ms, &year, &month, &day, &hour, &minute, &second, &micro);
    return year;
  }
  if (!v->IsString()) return std::nullopt;
  const std::string s = utf8(m_iso, v);
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
    if (j - i == 4) return std::stod(s.substr(i, 4));
    i = j == i ? i + 1 : j;
  }
  return std::nullopt;
}

void Values::write_as(Writer &wr, Local<Value> v, const ParamType &t) {
  if (v.IsEmpty() || v->IsNullOrUndefined()) {
    need(put_tag(wr, w::Tag::kNull));
    return;
  }
  const Local<v8::Context> ctx = context();
  const bool set_type = t.is("SET") || t.is("ENUM");
  if (set_type) {
    // Oracle's Table 27.7: a string for either, a Set for SET (its items' String() joined by
    // commas); anything else is a conversion error (MVP review JVML-11).
    std::string joined;
    if (v->IsString()) {
      joined = text(v);
    } else if (v->IsSet() && t.is("SET")) {
      const Local<v8::Array> items = v.As<v8::Set>()->AsArray();
      for (uint32_t i = 0; i < items->Length(); i++) {
        if (i > 0) joined += ',';
        joined += text(items->Get(ctx, i).ToLocalChecked());
      }
    } else {
      cannot(v, t);
    }
    need(put_tag(wr, w::Tag::kString) && wr.str(joined));
    return;
  }
  if (t.is("YEAR")) {
    if (const std::optional<double> y = year_of(v)) v = v8::Number::New(m_iso, *y);
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
        // Oracle: "Round value to Integer; check whether value is out of range", by Math.round:
        // the nearest integer, a half towards +Infinity (exact: d - floor(d) has no error).
        const double d = v.As<v8::Number>()->Value();
        if (!std::isfinite(d)) cannot(v, t);
        double r = std::floor(d);
        if (d - r >= 0.5) r += 1;
        if (!(std::fabs(r) < 0x1p100)) cannot(v, t);  // far outside every range; the range decides the rest
        b = static_cast<__int128>(r);
      } else if (v->IsString()) {
        // Oracle: "Parse as number and round to Integer", by Math.round as a Number is.
        const std::optional<Decimal> d = Decimal::parse(trim(utf8(m_iso, v)));
        if (!d) cannot(v, t);
        Decimal rounded = *d;
        rounded.round_to(0, true);
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
      } else if (v->IsBigInt()) {
        // Oracle: "Cast to Float; check whether result is out of range": Number(v), the nearest
        // double of its digits; beyond DOUBLE's range it is Infinity, which is out of range.
        d = std::strtod(str(v).c_str(), nullptr);
      } else if (v->IsString()) {
        const std::string s = trim(utf8(m_iso, v));
        // Oracle: "Parse value to Float"; a non-numeric string is an error. Decimal notation only:
        // what both JavaScript's Number() and Java's parseDouble read, without either's own forms
        // (0x10, 0b1, '' as 0; 1.5f, 2d, hex floats) or Infinity and NaN.
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
      // Oracle: "Convert to String" (a symbol too); what a toString throws is the routine's error.
      const Local<v8::String> s = string_of(v);
      need(put_tag(wr, w::Tag::kString) && write_utf8(m_iso, wr, s));
      return;
    }
    case w::Tag::kJson: {
      const std::optional<std::string> json = json_text(v);
      if (!json) cannot(v, t);
      need(put_tag(wr, w::Tag::kJson) && wr.str(*json));
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
      // Oracle: a typed array is treated as a Float32Array; so is an array (Float32Array.from(v),
      // as the GraalJS worker). 1 to 16383 dimensions (§7.2).
      const auto dimensions = [](uint64_t n) {
        if (n < 1 || n > w::kMaxVectorDimensions)
          throw ConversionError{"a VECTOR has 1 to " + std::to_string(w::kMaxVectorDimensions) + " dimensions, not " +
                                std::to_string(n)};
      };
      std::vector<float> f;
      if (v->IsFloat32Array()) {
        const Local<v8::Float32Array> a = v.As<v8::Float32Array>();
        dimensions(a->Length());
        f.resize(a->Length());
        a->CopyContents(f.data(), f.size() * 4);
      } else if (v->IsArray() || v->IsTypedArray()) {
        const Local<v8::Object> o = v.As<v8::Object>();
        const uint64_t n = v->IsArray() ? v.As<v8::Array>()->Length() : v.As<v8::TypedArray>()->Length();
        dimensions(n);
        f.resize(static_cast<size_t>(n));
        for (uint32_t i = 0; i < f.size(); i++) {
          // Each element's Number(): what a getter or valueOf throws (a BigInt's TypeError) stays
          // pending.
          Local<Value> e;
          Local<v8::Number> num;
          if (!o->Get(ctx, i).ToLocal(&e) || !e->ToNumber(ctx).ToLocal(&num)) throw GuestThrew{};
          f[i] = static_cast<float>(num->Value());  // the host checks that the elements are finite (§7.2)
        }
      } else {
        cannot(v, t);
      }
      need(put_tag(wr, w::Tag::kVector) && wr.put(static_cast<uint32_t>(f.size())) && wr.align() &&
           wr.bytes(std::as_bytes(std::span<const float>(f))));
      return;
    }
    case w::Tag::kTimestamp: {
      // The instant itself (§7.2), within the wire's range.
      const double ms = date_ms(v);
      if (!std::isfinite(ms)) cannot(v, t);
      if (ms < 0 || ms > static_cast<double>(w::kMaxTimestamp / 1000))
        throw ConversionError{"TIMESTAMP value out of range: " + instant_text(static_cast<int64_t>(ms))};
      need(put_tag(wr, w::Tag::kTimestamp) && wr.put(static_cast<int64_t>(ms) * 1000));
      return;
    }
    case w::Tag::kDate:
    case w::Tag::kDatetime: {
      double ms = date_ms(v);
      if (!std::isfinite(ms)) cannot(v, t);
      // Oracle (Table 27.3): to DATE "rounding off any time part to the closest second".
      if (t.tag == w::Tag::kDate) ms = std::floor((ms + 500) / 1000) * 1000;
      int year;
      unsigned month, day, hour, minute, second, micro;
      m_opt->local(ms, &year, &month, &day, &hour, &minute, &second, &micro);
      // The wire's year is 16 bits and MySQL's range ends at 9999 (a Date reaches 275760).
      if (year < 0 || year > 9999)
        throw ConversionError{"the year " + std::to_string(year) + " is out of range (0 to 9999)"};
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
      // Oracle: TIME is a string, validated by MySQL; here by its form and the wire's range.
      const std::string s = text(v);
      std::optional<Temporal> tm = parse_time(s);
      if (!tm) throw ConversionError{"Cannot convert value '" + s + "' to TIME from MLE"};
      if (tm->hour > w::kMaxTimeHour || (tm->hour == w::kMaxTimeHour && tm->minute == 59 && tm->second == 59 &&
                                          tm->micro > 0))
        throw ConversionError{"TIME value out of range: " + duration_text(*tm)};
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
    need(put_tag(wr, w::Tag::kString) && write_utf8(m_iso, wr, v.As<v8::String>()));
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
    // Any other object as JSON, by the rule of a JSON result (json_text); one without JSON (a
    // toJSON that returns nothing) cannot be bound, and what its toJSON throws stays pending.
    const std::optional<std::string> json = json_text(v);
    if (!json) {
      Local<v8::String> tag;  // Object.prototype.toString.call(v)
      if (!v.As<v8::Object>()->ObjectProtoToString(ctx).ToLocal(&tag)) throw GuestThrew{};
      throw ConversionError{"cannot bind " + utf8(m_iso, tag)};
    }
    need(put_tag(wr, w::Tag::kJson) && wr.str(*json));
  }
}

}  // namespace mle::v8w
