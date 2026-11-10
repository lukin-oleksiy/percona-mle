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

import java.time.DateTimeException;
import java.time.ZoneId;
import java.time.ZoneOffset;

/**
 * The session's conversion options (OPTIONS message): Oracle's integer_type and decimal_type for
 * JavaScript, and the time zone in which DATETIME and TIMESTAMP values become JavaScript Dates.
 */
final class Options {
  int integerType = Wire.INT_UNSAFE_STRING;
  int decimalType = Wire.DECIMAL_STRING;
  ZoneId zone = ZoneOffset.UTC;

  /** Parses an IANA name or an offset (+hh:mm); null if unknown. */
  static ZoneId parseZone(String text) {
    try {
      return ZoneId.of(text.equalsIgnoreCase("UTC") ? "Z" : text);
    } catch (DateTimeException e) {
      return null;
    }
  }
}
