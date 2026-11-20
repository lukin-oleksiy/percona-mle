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

/** A MySQL error from the SQL API ({@link Mle#session()}). Uncaught, it is raised to the caller unchanged. */
public class MleSqlException extends RuntimeException {
  @Serial private static final long serialVersionUID = 1L;

  private final int errorCode;
  private final String sqlState;
  private final long row;
  private final long errorId;

  public MleSqlException(int errorCode, String sqlState, String message) {
    this(errorCode, sqlState, message, -1);
  }

  /** row: the failing row of a batch (executeMany), from 0; -1 when the error is not about a row. */
  public MleSqlException(int errorCode, String sqlState, String message, long row) {
    this(errorCode, sqlState, message, row, 0);
  }

  /**
   * errorId: the id mysqld gave the error when a statement failed with it, by which the routine
   * hands the same error back when it ends with this exception; 0 for an error the routine raises.
   */
  public MleSqlException(int errorCode, String sqlState, String message, long row, long errorId) {
    super(message);
    this.errorCode = errorCode;
    this.sqlState = sqlState;
    this.row = row;
    this.errorId = errorId;
  }

  /** The id of the statement error this exception came from; 0 if it did not come from one. */
  public long getErrorId() {
    return errorId;
  }

  /** The failing row of a batch (executeMany), from 0; -1 when the error is not about a row of a batch. */
  public long getRow() {
    return row;
  }

  public int getErrorCode() {
    return errorCode;
  }

  public String getSqlState() {
    return sqlState;
  }
}
