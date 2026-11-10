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

package com.percona.mle.api;

import java.util.List;

/**
 * The result of {@link Mle#sql}: for a result set its column names and rows (values as Long,
 * BigInteger, Double, Float, BigDecimal, String, byte[], LocalDate, LocalDateTime, Duration,
 * MleJson, float[] or null); otherwise the affected rows and the last insert id.
 */
public record SqlResult(
    List<String> columns, List<Object[]> rows, long affectedRows, long insertId, List<String> warnings) {}
