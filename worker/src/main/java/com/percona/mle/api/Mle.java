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
 * The host API for routines running in an MLE worker.
 *
 * <p>This is the Phase 3 placeholder: {@link #sql} runs one statement in the calling session (in
 * its transaction, with the routine's privileges) and returns the whole result. The full SQL API
 * comes with Phase 5.
 */
public final class Mle {
  private Mle() {}

  /**
   * Runs a statement with ? placeholders bound to params (Long, Integer, Double, Float,
   * BigDecimal, BigInteger, String, byte[], LocalDate, LocalDateTime, LocalTime, Duration,
   * MleJson, float[] for VECTOR, Boolean, or null). Must be called on the routine's thread.
   *
   * @throws MleSqlException if the statement fails
   */
  public static SqlResult sql(String statement, Object... params) {
    return SqlBridge.sql(statement, params);
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
