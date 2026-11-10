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

package com.percona.mle.sql;

/**
 * How a language sees SQL values: the values of rows and user variables, and the parameters it
 * binds. Java routines get plain Java values; the JavaScript and Python adapters map to their
 * languages' values (the worker provides the mappers).
 */
public interface ValueMapper {
  /**
   * A value as read from a result (Long, BigInteger, Double, Float, BigDecimal, String, byte[],
   * com.percona.mle.api.MleJson, float[] for VECTOR, a worker temporal for DATE/TIME/DATETIME/
   * TIMESTAMP, or null) in this language, for column c under the statement's options.
   */
  Object value(Object raw, Column c, SqlOptions options);

  /**
   * A parameter given by the routine as a value to bind (Long, Double, BigInteger, BigDecimal,
   * String, byte[], float[], MleJson, LocalDate, LocalDateTime, Duration, Boolean, or null).
   */
  Object param(Object value);
}
