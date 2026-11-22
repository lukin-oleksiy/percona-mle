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

package com.percona.mle.worker;

import java.time.Instant;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.zone.ZoneOffsetTransition;
import java.time.zone.ZoneRules;
import java.util.List;

/**
 * Instants and local times in the zone of OPTIONS (Appendix A, TIMESTAMP): the interval of the
 * zone's current offset, from one transition to the next, is kept, so that a value inside it costs
 * a compare and an add; only a value outside it looks the zone up (ZoneRules.getOffset per value
 * cost a Java routine 16 % per TIMESTAMP row in Europe/Vilnius, protocol-review.md §17). For local
 * time to instant, the interval leaves out the local times its bounding transitions repeat or
 * skip: those go to Java's rules (a repeated local time is its first occurrence, a skipped one is
 * moved forward by the gap, as JavaScript's Date constructor does). Used on the worker's main
 * thread only.
 */
final class ZoneCache {
  final ZoneId zone;
  private final ZoneRules rules;
  private final boolean fixed;
  private final int fixedOffset;

  /** Instant to local time: [from, until) in epoch seconds has offset (seconds). Empty at first. */
  private long from = 1, until = 0;
  private int offset;
  /** [from, foldUntil): local times the transition at from repeats, the second time (Python's fold 1). */
  private long foldUntil;

  /** Local time to instant: local epoch seconds [localFrom, localUntil) have localOffset. */
  private long localFrom = 1, localUntil = 0;
  private int localOffset;

  ZoneCache(ZoneId zone) {
    this.zone = zone;
    this.rules = zone.getRules();
    this.fixed = rules.isFixedOffset();
    this.fixedOffset = fixed ? rules.getOffset(Instant.EPOCH).getTotalSeconds() : 0;
  }

  /** The zone's offset (seconds) at the instant epochSecond. */
  int offsetAt(long epochSecond) {
    if (fixed) return fixedOffset;
    if (epochSecond >= from && epochSecond < until) return offset;
    return offsetMiss(epochSecond);
  }

  private int offsetMiss(long epochSecond) {
    Instant i = Instant.ofEpochSecond(epochSecond);
    int o = rules.getOffset(i).getTotalSeconds();
    // Transitions are on whole seconds: the one at or before i, and the first after it.
    ZoneOffsetTransition prev = rules.previousTransition(i.plusSeconds(1));
    ZoneOffsetTransition next = rules.nextTransition(i);
    from = prev == null ? Long.MIN_VALUE : prev.toEpochSecond();
    until = next == null ? Long.MAX_VALUE : next.toEpochSecond();
    offset = o;
    foldUntil = prev == null ? from : from + Math.max(0, prev.getOffsetBefore().getTotalSeconds() - o);
    return o;
  }

  /**
   * Whether the instant's local time is the second of two that a transition repeats (Python's
   * fold = 1, as zoneinfo's fromutc sets it).
   */
  boolean fold(long epochSecond) {
    if (fixed) return false;
    offsetAt(epochSecond);
    return epochSecond < foldUntil;
  }

  /** The local time (epoch seconds of the local date and time) of the instant epochSecond. */
  long local(long epochSecond) {
    return epochSecond + offsetAt(epochSecond);
  }

  /** The local date and time of the instant (epochSecond, nano). */
  LocalDateTime localDateTime(long epochSecond, int nano) {
    return LocalDateTime.ofEpochSecond(epochSecond + offsetAt(epochSecond), nano, ZoneOffset.UTC);
  }

  /** The instant (epoch seconds) of a local time given as local epoch seconds. */
  long instant(long localSecond) {
    if (fixed) return localSecond - fixedOffset;
    if (localSecond >= localFrom && localSecond < localUntil) return localSecond - localOffset;
    return instantMiss(localSecond);
  }

  private long instantMiss(long localSecond) {
    LocalDateTime ldt = LocalDateTime.ofEpochSecond(localSecond, 0, ZoneOffset.UTC);
    List<ZoneOffset> valid = rules.getValidOffsets(ldt);
    if (valid.size() != 1) return ldt.atZone(zone).toEpochSecond();  // repeated or skipped: Java's rules
    int o = valid.get(0).getTotalSeconds();
    long epochSecond = localSecond - o;
    Instant i = Instant.ofEpochSecond(epochSecond);
    ZoneOffsetTransition prev = rules.previousTransition(i.plusSeconds(1));
    ZoneOffsetTransition next = rules.nextTransition(i);
    localFrom = prev == null ? Long.MIN_VALUE
        : prev.toEpochSecond() + Math.max(prev.getOffsetBefore().getTotalSeconds(), o);
    localUntil = next == null ? Long.MAX_VALUE
        : next.toEpochSecond() + Math.min(next.getOffsetAfter().getTotalSeconds(), o);
    localOffset = o;
    return epochSecond;
  }
}
