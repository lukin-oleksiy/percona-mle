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

#ifndef COMPONENTS_MLE_COMPONENT_TIME_ZONE_HPP
#define COMPONENTS_MLE_COMPONENT_TIME_ZONE_HPP

/**
  @file component/time_zone.hpp

  Time zones of TIMESTAMP values (protocol §7.2, Appendix A "TIMESTAMP").
  The server gives a TIMESTAMP as local time in the session's zone; the
  protocol carries the instant. A TimeZone turns a local time into the
  instant:

  * an offset ("+05:30"): exactly, for all time;
  * a named zone: by the system's zone data (std::chrono);
  * SYSTEM: by mysqld's zone's IANA name if it has one, else by the C
    library's local time, from which the transitions are derived.

  A local time that the zone repeats is taken as its first occurrence, one
  that it skips as the instant of the change (§7.2, the server's rules).
  The TimeZone keeps the interval of local time in which the zone has the
  offset found last, between two transitions and without the local times
  that those transitions repeat or skip: a value inside it costs a compare
  and a subtraction; only one outside it looks the zone up.

  No MySQL dependencies: unit-tested on its own.
*/

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "mle/value.h"

namespace mle_component {

/**
  mysqld's system time zone (time_zone = SYSTEM), as Java and the workers
  understand it: an IANA name (TZ=Area/City or /etc/localtime), else the
  current UTC offset "+hh:mm" (POSIX TZ strings like GMT-3 mean UTC+3 to
  libc but UTC-3 to Java).
*/
std::string system_time_zone();

/** A session's time zone as OPTIONS names it (§9.8): an offset "+hh:mm" or
    an IANA name; SYSTEM as mysqld's own zone (system_time_zone()). Empty
    for a zone the protocol cannot express: a name that is too long, a zone
    that counts leap seconds. */
std::string options_time_zone(std::string_view session_zone);

class TimeZone {
 public:
  /** name as the server names the session's zone: "SYSTEM", an offset or a
      zone name. False for a zone that is not known here (the system's zone
      data has no such name). */
  bool set(std::string_view name);
  /** The instant of a local time in this zone, in microseconds since
      1970-01-01 00:00:00 UTC; MySQL's zero timestamp for a zero local time.
      False if the fields are no date or the instant is outside the
      protocol's range. */
  bool instant(const mle::Temporal &local, int64_t *microseconds);
  /** The local time of an instant (microseconds since 1970 UTC, not the
      zero timestamp) in this zone: how a TIMESTAMP reaches the server
      (§7.2). False for an instant outside the years 1-9999. It keeps the
      span of instants with the offset found last, as instant() does. */
  bool local(int64_t microseconds, mle::Temporal *local);
  /** Takes the interval other has found, if other is this zone. */
  void learn(const TimeZone &other) {
    if (other.m_id != m_id) return;
    m_from = other.m_from;
    m_to = other.m_to;
    m_offset = other.m_offset;
  }
  /** For tests: the interval of local time (seconds since the epoch on the
      zone's clock) whose instants are local - offset() without a lookup. */
  int64_t from() const { return m_from; }
  int64_t to() const { return m_to; }
  int64_t offset() const { return m_offset; }

 private:
  /** A span of instants [begin, end) with one offset; begin INT64_MIN or
      end INT64_MAX: none known on that side. */
  struct Period {
    int64_t begin = 0;
    int64_t end = 0;
    int64_t offset = 0;
  };
  /** The period that holds the instant (seconds since the epoch, UTC). */
  bool period_at(int64_t instant, Period *p) const;
  /** The offset at an instant. */
  bool offset_at(int64_t instant, int64_t *offset) const;
  /** The instant, in seconds, of local (seconds since the epoch on the
      zone's clock) by the zone's rules; keeps the interval of the period it
      finds. */
  bool lookup(int64_t local, int64_t *seconds);

  /** The rules: a zone of the zone data; or, for SYSTEM without one, the C
      library's (m_libc); unused while the interval is everything. */
  const std::chrono::time_zone *m_zone = nullptr;
  bool m_libc = false;
  /** Local times in [m_from, m_to) are the instants local - m_offset. */
  int64_t m_from = 0;
  int64_t m_to = 0;
  int64_t m_offset = 0;
  /** Instants in [m_at_from, m_at_to) (seconds) have the offset m_at_offset. */
  int64_t m_at_from = 0;
  int64_t m_at_to = 0;
  int64_t m_at_offset = 0;
  /** Which zone this is: copies of one set() share it. */
  uint64_t m_id = 0;
};

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_TIME_ZONE_HPP
