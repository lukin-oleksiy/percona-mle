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

// TIMESTAMP values from local time (docs/protocol.md §7.2, Appendix A
// "TIMESTAMP"), as component_mle converts them (component/time_zone.hpp):
// the zone's offset interval is cached without the local times its bounding
// transitions repeat or skip, so a repeated local time is still its first
// occurrence and a skipped one the instant of the change; SYSTEM without an
// IANA name derives the transitions from the C library.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "time_zone.hpp"

namespace mle_time_zone_unittest {

namespace ch = std::chrono;
using mle_component::TimeZone;

constexpr int64_t kHour = 3600;

/** Seconds since the epoch on the zone's clock. */
int64_t local_of(int y, unsigned mo, unsigned d, unsigned h, unsigned mi = 0,
                 unsigned s = 0) {
  const ch::local_days day{ch::year{y} / ch::month{mo} / ch::day{d}};
  return day.time_since_epoch().count() * int64_t{86400} + h * kHour + mi * 60 + s;
}

int64_t utc_of(int y, unsigned mo, unsigned d, unsigned h, unsigned mi = 0) {
  return local_of(y, mo, d, h, mi);
}

mle::Temporal fields(int64_t local) {
  const ch::local_seconds at{ch::seconds{local}};
  const auto day = ch::floor<ch::days>(at);
  const ch::year_month_day date{day};
  const ch::hh_mm_ss<ch::seconds> time{at - day};
  mle::Temporal t;
  t.year = static_cast<uint16_t>(static_cast<int>(date.year()));
  t.month = static_cast<uint8_t>(static_cast<unsigned>(date.month()));
  t.day = static_cast<uint8_t>(static_cast<unsigned>(date.day()));
  t.hour = static_cast<uint32_t>(time.hours().count());
  t.minute = static_cast<uint8_t>(time.minutes().count());
  t.second = static_cast<uint8_t>(time.seconds().count());
  return t;
}

/** The instant in seconds, or a failure. */
int64_t convert(TimeZone &zone, int64_t local) {
  int64_t us = 0;
  EXPECT_TRUE(zone.instant(fields(local), &us)) << local;
  return us / 1000000;
}

/** §7.2 by the zone data, without a cache: a repeated local time is its
    first occurrence, a skipped one the instant of the change. */
int64_t reference(const ch::time_zone *zone, int64_t local) {
  const ch::local_info info = zone->get_info(ch::local_seconds{ch::seconds{local}});
  switch (info.result) {
    case ch::local_info::unique:
    case ch::local_info::ambiguous:
      return local - info.first.offset.count();
    default:
      return info.first.end.time_since_epoch().count();
  }
}

/** Every step minutes over two years, and every minute of the three hours
    on each side of a transition, in order and then shuffled, through one
    TimeZone (its cached interval in use), against the reference. */
void agrees_with(TimeZone &zone, const ch::time_zone *rules, int year,
                 int step = 7) {
  std::vector<int64_t> locals;
  const int64_t first = local_of(year, 1, 1, 0), last = local_of(year + 2, 1, 1, 0);
  for (int64_t l = first; l < last; l += step * 60) locals.push_back(l);
  for (ch::sys_seconds t{ch::seconds{first - 86400}};;) {
    const ch::sys_info info = rules->get_info(t);
    const int64_t change = info.end.time_since_epoch().count();
    if (change > last) break;
    for (int64_t l = change + info.offset.count() - 3 * kHour;
         l < change + info.offset.count() + 3 * kHour; l += 60)
      locals.push_back(l);
    t = info.end;
  }
  for (int pass = 0; pass < 2; pass++) {
    for (const int64_t l : locals) {
      ASSERT_EQ(convert(zone, l), reference(rules, l)) << "local " << l;
    }
    std::shuffle(locals.begin(), locals.end(), std::mt19937(42));
  }
}

/** Europe/Vilnius 2024: EET (+2) to EEST (+3) at 01:00 UTC on 31 March (local
    03:00-03:59 skipped), back at 01:00 UTC on 27 October (local 03:00-03:59
    repeated). */
void vilnius_2024(TimeZone &zone) {
  // Summer: the interval is from the spring change, without the skipped
  // hour, to the autumn one, without the repeated hour.
  EXPECT_EQ(convert(zone, local_of(2024, 6, 1, 12)), utc_of(2024, 6, 1, 9));
  EXPECT_EQ(zone.offset(), 3 * kHour);
  EXPECT_EQ(zone.from(), local_of(2024, 3, 31, 4));
  EXPECT_EQ(zone.to(), local_of(2024, 10, 27, 3));
  // The last local time before the repeated hour: from the interval.
  EXPECT_EQ(convert(zone, local_of(2024, 10, 27, 2, 59, 59)),
            utc_of(2024, 10, 26, 23, 59) + 59);
  // The repeated hour: its first occurrence (+3), and the interval does not
  // take it in.
  EXPECT_EQ(convert(zone, local_of(2024, 10, 27, 3, 30)), utc_of(2024, 10, 27, 0, 30));
  EXPECT_LE(zone.to(), local_of(2024, 10, 27, 3));
  EXPECT_EQ(convert(zone, local_of(2024, 10, 27, 3, 0)), utc_of(2024, 10, 27, 0, 0));
  EXPECT_EQ(convert(zone, local_of(2024, 10, 27, 3, 59, 59)),
            utc_of(2024, 10, 27, 0, 59) + 59);
  // After it: winter, +2.
  EXPECT_EQ(convert(zone, local_of(2024, 10, 27, 4, 0)), utc_of(2024, 10, 27, 2, 0));
  EXPECT_EQ(zone.offset(), 2 * kHour);
  EXPECT_EQ(zone.from(), local_of(2024, 10, 27, 4));
  // The skipped hour: the instant of the change.
  EXPECT_EQ(convert(zone, local_of(2024, 3, 31, 3, 0)), utc_of(2024, 3, 31, 1, 0));
  EXPECT_EQ(convert(zone, local_of(2024, 3, 31, 3, 30)), utc_of(2024, 3, 31, 1, 0));
  EXPECT_EQ(convert(zone, local_of(2024, 3, 31, 2, 59, 59)),
            utc_of(2024, 3, 31, 0, 59) + 59);
  EXPECT_EQ(convert(zone, local_of(2024, 3, 31, 4, 0)), utc_of(2024, 3, 31, 1, 0));
  // Winter before the spring change: the interval ends at the skipped hour.
  EXPECT_EQ(convert(zone, local_of(2024, 2, 1, 12)), utc_of(2024, 2, 1, 10));
  EXPECT_EQ(zone.to(), local_of(2024, 3, 31, 3));
}

/** The local time of an instant, in seconds on the zone's clock. */
int64_t local_seconds(TimeZone &zone, int64_t utc) {
  mle::Temporal t;
  EXPECT_TRUE(zone.local(utc * 1000000, &t)) << utc;
  return local_of(t.year, t.month, t.day, t.hour, t.minute, t.second);
}

/** Instants to local time (how a TIMESTAMP parameter reaches the server):
    the repeated hour is shown twice, the skipped one never. */
void vilnius_local(TimeZone &zone) {
  EXPECT_EQ(local_seconds(zone, utc_of(2024, 10, 27, 0, 30)), local_of(2024, 10, 27, 3, 30));
  EXPECT_EQ(local_seconds(zone, utc_of(2024, 10, 27, 1, 30)), local_of(2024, 10, 27, 3, 30));
  EXPECT_EQ(local_seconds(zone, utc_of(2024, 3, 31, 0, 59) + 59), local_of(2024, 3, 31, 2, 59, 59));
  EXPECT_EQ(local_seconds(zone, utc_of(2024, 3, 31, 1, 0)), local_of(2024, 3, 31, 4, 0));
  EXPECT_EQ(local_seconds(zone, utc_of(2024, 6, 1, 9)), local_of(2024, 6, 1, 12));
  mle::Temporal t;
  ASSERT_TRUE(zone.local(utc_of(2024, 6, 1, 9) * 1000000 + 123456, &t));
  EXPECT_EQ(t.microsecond, 123456u);
  const ch::time_zone *rules = ch::locate_zone("Europe/Vilnius");
  for (int64_t at = utc_of(2024, 1, 1, 0); at < utc_of(2026, 1, 1, 0); at += 3593)
    ASSERT_EQ(local_seconds(zone, at), rules->to_local(ch::sys_seconds{ch::seconds{at}})
                                           .time_since_epoch()
                                           .count())
        << at;
}

TEST(MleTimeZone, Offset) {
  TimeZone zone;
  ASSERT_TRUE(zone.set("+05:30"));
  EXPECT_EQ(convert(zone, local_of(2024, 6, 1, 12)), utc_of(2024, 6, 1, 6, 30));
  EXPECT_EQ(zone.from(), INT64_MIN);
  EXPECT_EQ(zone.to(), INT64_MAX);
  ASSERT_TRUE(zone.set("-13:59"));
  EXPECT_FALSE(zone.set("+14:01"));
}

TEST(MleTimeZone, NamedZoneAtTransitions) {
  TimeZone zone;
  ASSERT_TRUE(zone.set("Europe/Vilnius"));
  vilnius_2024(zone);
  TimeZone fresh;
  ASSERT_TRUE(fresh.set("Europe/Vilnius"));
  agrees_with(fresh, ch::locate_zone("Europe/Vilnius"), 2024);
  TimeZone instants;
  ASSERT_TRUE(instants.set("Europe/Vilnius"));
  vilnius_local(instants);
}

TEST(MleTimeZone, NamedZonesAgreeWithTheZoneData) {
  // Southern summer time; a half-hour change; a day the zone skipped
  // (Apia, 30 December 2011); a zone that changed its standard offset.
  for (const char *name : {"Australia/Sydney", "Australia/Lord_Howe",
                           "America/New_York", "Pacific/Apia", "Europe/Moscow"}) {
    SCOPED_TRACE(name);
    TimeZone zone;
    ASSERT_TRUE(zone.set(name));
    agrees_with(zone, ch::locate_zone(name), 2010);
  }
  TimeZone apia;
  ASSERT_TRUE(apia.set("Pacific/Apia"));
  // The whole of 30 December 2011 was skipped: the instant of the change.
  const ch::time_zone *apia_rules = ch::locate_zone("Pacific/Apia");
  const int64_t noon = local_of(2011, 12, 30, 12);
  const ch::local_info skipped =
      apia_rules->get_info(ch::local_seconds{ch::seconds{noon}});
  ASSERT_EQ(skipped.result, ch::local_info::nonexistent);
  EXPECT_EQ(convert(apia, noon), skipped.first.end.time_since_epoch().count());
  EXPECT_EQ(convert(apia, local_of(2011, 12, 31, 0)),
            skipped.first.end.time_since_epoch().count());
  TimeZone unknown;
  EXPECT_FALSE(unknown.set("Nowhere/Atlantis"));
}

/** SYSTEM without an IANA name: the C library's local time, by a POSIX rule
    that is Vilnius's since 1997. */
class MleSystemZone : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char *tz = getenv("TZ"); tz != nullptr) m_saved = tz, m_had = true;
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
  }
  void TearDown() override {
    if (m_had)
      setenv("TZ", m_saved.c_str(), 1);
    else
      unsetenv("TZ");
    tzset();
  }

 private:
  std::string m_saved;
  bool m_had = false;
};

TEST_F(MleSystemZone, TransitionsFromTheCLibrary) {
  // Not an IANA name: SYSTEM is sent to workers as the offset now.
  const std::string system = mle_component::system_time_zone();
  EXPECT_TRUE(system == "+02:00" || system == "+03:00") << system;
  TimeZone zone;
  ASSERT_TRUE(zone.set("SYSTEM"));
  vilnius_2024(zone);
  TimeZone fresh;
  ASSERT_TRUE(fresh.set("SYSTEM"));
  agrees_with(fresh, ch::locate_zone("Europe/Vilnius"), 2024, 61);
  TimeZone instants;
  ASSERT_TRUE(instants.set("SYSTEM"));
  vilnius_local(instants);
}

}  // namespace mle_time_zone_unittest
