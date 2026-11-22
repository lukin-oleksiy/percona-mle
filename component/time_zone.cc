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

#include "time_zone.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

#include "mle/codec.h"

namespace mle_component {

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

/** The zone name of a tzfile path ("/usr/share/zoneinfo/Europe/Berlin", or a
    link to one such as /etc/localtime), or "". */
std::string zone_of_file(const std::string &file) {
  const auto zone_in = [](const std::string &path) {
    const size_t at = path.find("zoneinfo/");
    return at == std::string::npos ? std::string() : path.substr(at + 9);
  };
  // The name as configured first (TZ=:/usr/share/zoneinfo/US/Eastern),
  // then the link's target (/etc/localtime), then the file itself (a
  // relative link inside zoneinfo, e.g. Europe/Kiev -> Kyiv).
  if (std::string zone = zone_in(file); !zone.empty()) return zone;
  char target[PATH_MAX];
  if (const ssize_t n = readlink(file.c_str(), target, sizeof target - 1); n > 0) {
    if (std::string zone = zone_in(std::string(target, static_cast<size_t>(n)));
        !zone.empty())
      return zone;
  }
  if (char real[PATH_MAX]; realpath(file.c_str(), real) != nullptr)
    return zone_in(real);
  return "";
}

std::atomic<uint64_t> g_zone_ids{0};

/** Beyond this, a period of the zone data has no end on that side. */
constexpr int64_t kFar = INT64_MAX / 4;

// The C library's local time (SYSTEM without an IANA name).
constexpr int64_t kDay = 86400;
/** How far the transitions around an instant are looked for, each way: a
    period found longer than that is cut there (and looked up again
    beyond). */
constexpr int64_t kScanDays = 400;

bool libc_offset(int64_t instant, int64_t *offset) {
  const time_t at = static_cast<time_t>(instant);
  struct tm tm {};
  if (localtime_r(&at, &tm) == nullptr) return false;
  *offset = tm.tm_gmtoff;
  return true;
}

/** The first instant in (lo, hi] whose offset is not the one at lo, given
    that the one at hi is not (one change between them). */
bool libc_change(int64_t lo, int64_t hi, int64_t offset_lo, int64_t *at) {
  while (hi - lo > 1) {
    const int64_t mid = lo + (hi - lo) / 2;
    int64_t o = 0;
    if (!libc_offset(mid, &o)) return false;
    (o == offset_lo ? lo : hi) = mid;
  }
  *at = hi;
  return true;
}

}  // namespace

std::string system_time_zone() {
  std::string file = "/etc/localtime";
  if (const char *tz = getenv("TZ"); tz != nullptr && *tz != '\0') {
    const std::string zone = tz[0] == ':' ? tz + 1 : tz;
    if (zone == "UTC") return zone;
    if (zone[0] != '/') {
      // Area/City; a POSIX rule has a ',' before any '/'
      // (EET-2EEST,M3.5.0/3,M10.5.0/4).
      if (zone.find('/') != std::string::npos &&
          zone.find(',') == std::string::npos)
        return zone;
      file.clear();  // a POSIX rule such as CET-1CEST: the offset below
    } else {
      file = zone;  // TZ=:/etc/localtime (systemd units, containers)
    }
  }
  if (!file.empty()) {
    if (std::string zone = zone_of_file(file); !zone.empty()) return zone;
  }
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  const long off = local.tm_gmtoff;
  char b[48];
  snprintf(b, sizeof b, "%c%02ld:%02ld", off < 0 ? '-' : '+', labs(off) / 3600,
           labs(off) % 3600 / 60);
  return b;
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

bool TimeZone::set(std::string_view name) {
  m_id = ++g_zone_ids;
  m_zone = nullptr;
  m_libc = false;
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
  // one, else by the C library.
  const std::string system = name == "SYSTEM" ? system_time_zone() : "";
  if (name == "SYSTEM") {
    int32_t offset = 0;
    if (system.empty() || parse_offset(system, &offset)) {
      m_libc = true;
      return true;
    }
    name = system;
  }
  try {
    m_zone = std::chrono::locate_zone(name);
  } catch (const std::runtime_error &) {
    // No such zone, or no zone data on this host: for SYSTEM the C library
    // still converts.
    m_libc = !system.empty();
    return m_libc;
  }
  return true;
}

bool TimeZone::offset_at(int64_t instant, int64_t *offset) const {
  if (m_libc) return libc_offset(instant, offset);
  if (m_zone == nullptr) return false;
  namespace ch = std::chrono;
  *offset = m_zone->get_info(ch::sys_seconds{ch::seconds{instant}}).offset.count();
  return true;
}

bool TimeZone::period_at(int64_t instant, Period *p) const {
  if (m_zone != nullptr) {
    namespace ch = std::chrono;
    const ch::sys_info info = m_zone->get_info(ch::sys_seconds{ch::seconds{instant}});
    const int64_t begin = info.begin.time_since_epoch().count();
    const int64_t end = info.end.time_since_epoch().count();
    p->begin = begin < -kFar ? INT64_MIN : begin;
    p->end = end > kFar ? INT64_MAX : end;
    p->offset = info.offset.count();
    return true;
  }
  if (!m_libc) return false;
  // The transitions, from the C library's local time: the offset is looked
  // at a day apart, and a change between two looks is found to the second.
  int64_t offset = 0;
  if (!libc_offset(instant, &offset)) return false;
  p->offset = offset;
  p->end = instant + kScanDays * kDay;
  for (int64_t day = 1; day <= kScanDays; day++) {
    int64_t o = 0;
    const int64_t at = instant + day * kDay;
    if (!libc_offset(at, &o)) return false;
    if (o != offset) {
      if (!libc_change(at - kDay, at, offset, &p->end)) return false;
      break;
    }
  }
  p->begin = instant - kScanDays * kDay;
  for (int64_t day = 1; day <= kScanDays; day++) {
    int64_t o = 0;
    const int64_t at = instant - day * kDay;
    if (!libc_offset(at, &o)) return false;
    if (o != offset) {
      // The first instant after at with this offset.
      if (!libc_change(at, at + kDay, o, &p->begin)) return false;
      break;
    }
  }
  return true;
}

bool TimeZone::lookup(int64_t local, int64_t *seconds) {
  // Offsets are within a day each way: the period at local - offset(local)
  // is the one that holds local, or a neighbour of it.
  int64_t guess = 0;
  Period p;
  if (!offset_at(local, &guess) || !period_at(local - guess, &p)) return false;
  for (int step = 0; step < 8; step++) {
    const bool has_prev = p.begin != INT64_MIN;
    const bool has_next = p.end != INT64_MAX;
    // The offsets of the neighbours (p's own where it has none).
    int64_t prev = p.offset, next = p.offset;
    if ((has_prev && !offset_at(p.begin - 1, &prev)) ||
        (has_next && !offset_at(p.end, &next)))
      return false;
    if (has_prev && local < p.begin + p.offset) {
      // The zone skips the local times from the previous period's end to
      // this one's start: such a time is the instant of the change.
      if (local >= p.begin + prev) {
        *seconds = p.begin;
        return true;
      }
      if (!period_at(p.begin - 1, &p)) return false;
      continue;
    }
    if (has_next && local >= p.end + p.offset) {
      if (local < p.end + next) {
        *seconds = p.end;
        return true;
      }
      if (!period_at(p.end, &p)) return false;
      continue;
    }
    // local is one of p's local times. One that the previous period has too
    // (the zone repeats it): its first occurrence, in that period.
    if (has_prev && local < p.begin + prev) {
      *seconds = local - prev;
      return true;
    }
    *seconds = local - p.offset;
    // The interval: p's local times without those its transitions repeat or
    // skip (a time p shares with the next period is p's, the first, but is
    // left out too: a compare must decide nothing more).
    m_from = has_prev ? p.begin + std::max(p.offset, prev) : INT64_MIN;
    m_to = has_next ? p.end + std::min(p.offset, next) : INT64_MAX;
    m_offset = p.offset;
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
  if ((local < m_from || local >= m_to) && !lookup(local, &seconds))
    return false;
  *microseconds = seconds * 1000000 + t.microsecond;
  return mle::is_valid_timestamp(*microseconds) &&
         *microseconds != mle::wire::kZeroTimestamp;
}

}  // namespace mle_component
