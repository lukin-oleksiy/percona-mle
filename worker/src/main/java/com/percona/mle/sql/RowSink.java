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
 * Takes the rows of a result as they are decoded from the mailbox (exp note 44 step 3), so that a
 * language copies them straight into its own arrays: no row objects, no float[] per VECTOR.
 */
public interface RowSink {
  /**
   * One row's raw values (see ValueMapper#value); a VECTOR is a float[] or a read-only
   * java.lang.foreign.MemorySegment of little-endian floats, valid only during this call. The
   * array is reused for the next row. It must not run SQL: the result is still being read.
   */
  void row(Object[] values);
}
