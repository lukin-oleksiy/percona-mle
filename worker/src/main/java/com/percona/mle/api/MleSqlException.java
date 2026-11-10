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

import java.io.Serial;

/** A MySQL error from {@link Mle#sql}. Uncaught, it is raised to the caller unchanged. */
public class MleSqlException extends RuntimeException {
  @Serial private static final long serialVersionUID = 1L;

  private final int errorCode;
  private final String sqlState;

  public MleSqlException(int errorCode, String sqlState, String message) {
    super(message);
    this.errorCode = errorCode;
    this.sqlState = sqlState;
  }

  public int getErrorCode() {
    return errorCode;
  }

  public String getSqlState() {
    return sqlState;
  }
}
