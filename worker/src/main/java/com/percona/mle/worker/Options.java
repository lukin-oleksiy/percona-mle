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
 * The session's options (OPTIONS, §9.8): the time zone, in which the languages show local time
 * (DATETIME and TIMESTAMP values as JavaScript Dates and Python datetimes), and the settings this
 * worker knows (Appendix B.2): js.integer_type and js.decimal_type, how JavaScript receives
 * integers and DECIMAL. A setting that OPTIONS does not carry has its default.
 */
final class Options {
  static final String INTEGER_TYPE = "js.integer_type";
  static final String DECIMAL_TYPE = "js.decimal_type";

  // js.integer_type: a number when safe (within 2^53 - 1) and else a string or a BigInt; always
  // a string; always a BigInt. The numbers are those of SqlOptions.INTEGER_TYPES.
  static final int INT_UNSAFE_STRING = 0;
  static final int INT_UNSAFE_BIGINT = 1;
  static final int INT_STRING = 2;
  static final int INT_BIGINT = 3;
  // js.decimal_type (SqlOptions.DECIMAL_TYPES).
  static final int DECIMAL_STRING = 0;
  static final int DECIMAL_NUMBER = 1;

  int integerType = INT_UNSAFE_STRING;
  int decimalType = DECIMAL_STRING;
  ZoneId zone = ZoneOffset.UTC;

  /** Back to the defaults (RESET ends the options in force). */
  void reset() {
    integerType = INT_UNSAFE_STRING;
    decimalType = DECIMAL_STRING;
    zone = ZoneOffset.UTC;
  }

  /** Parses an IANA name or an offset (+hh:mm); null if unknown. */
  static ZoneId parseZone(String text) {
    try {
      return ZoneId.of(text.equalsIgnoreCase("UTC") ? "Z" : text);
    } catch (DateTimeException e) {
      return null;
    }
  }

  /** A value of js.integer_type as its number, or -1 for a value that is none. */
  static int integerType(String value) {
    return switch (value) {
      case "unsafe_string" -> INT_UNSAFE_STRING;
      case "unsafe_bigint" -> INT_UNSAFE_BIGINT;
      case "string" -> INT_STRING;
      case "bigint" -> INT_BIGINT;
      default -> -1;
    };
  }

  /** A value of js.decimal_type as its number, or -1 for a value that is none. */
  static int decimalType(String value) {
    return switch (value) {
      case "string" -> DECIMAL_STRING;
      case "number" -> DECIMAL_NUMBER;
      default -> -1;
    };
  }
}
