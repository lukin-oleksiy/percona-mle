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

import com.percona.mle.sql.Column;
import com.percona.mle.sql.SqlOptions;
import com.percona.mle.sql.ValueMapper;

/**
 * Values for Java routines (Mle.session()): Long, BigInteger, Double, Float, BigDecimal, String,
 * byte[], MleJson, float[] (VECTOR), LocalDate, LocalDateTime, Duration (TIME); parameters as
 * Java values. A zero date cannot be read as a Java date (a conversion error).
 */
final class JavaSql implements ValueMapper {
  @Override
  public Object value(Object raw, Column c, SqlOptions o) {
    return Values.toJava(raw);
  }

  @Override
  public Object param(Object v) {
    return v;
  }
}
