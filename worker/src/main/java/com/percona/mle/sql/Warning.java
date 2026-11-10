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
 * A warning of the statement last run (Oracle's Warning: level 1 NOTE, 2 WARNING, 3 ERROR; the
 * MySQL error code; the message).
 */
public record Warning(int level, int code, String message) {
  public int getLevel() {
    return level;
  }

  public int getCode() {
    return code;
  }

  public String getMessage() {
    return message;
  }
}
