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

import com.percona.mle.worker.SqlBridge;

/**
 * The host API for routines running in an MLE worker: the SQL session ({@link #session()}, Oracle's
 * SQL API, com.percona.mle.sql) and the polyglot context ({@link #context()}).
 */
public final class Mle {
  private Mle() {}

  /**
   * The session for SQL from Java routines (the calling session, its transaction, the routine's
   * privileges): Oracle's object model with Java values (Long, BigInteger, Double, Float,
   * BigDecimal, String, byte[], MleJson, float[] for VECTOR, LocalDate, LocalDateTime, Duration).
   * Must be used on the routine's thread. Errors are {@link MleSqlException}s.
   */
  public static com.percona.mle.sql.Session session() {
    return SqlBridge.javaSession();
  }

  /**
   * The worker's polyglot Context (JS and Python), for calls from Java routines into the other
   * languages, e.g. {@code Mle.context().eval("python", "import numpy; numpy.dot")}. Must be used
   * on the routine's thread.
   */
  public static org.graalvm.polyglot.Context context() {
    return SqlBridge.context();
  }
}
