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

/**
 * Groups of similar rows ({@link VectorSet#groups}, {@link Vectors#groups}): the connected
 * components of the "at least this similar" graph (single linkage: a chain of close pairs joins
 * its ends, so at low thresholds groups can grow large). Largest first, then by first member;
 * members in row order. JSON with {@code String(g)} / {@code str(g)}.
 */
public final class Groups {
  private final int[][] members;
  private final long[] ids;

  Groups(int[][] members, long[] ids) {
    this.members = members;
    this.ids = ids;
  }

  /** How many groups. */
  public int size() {
    return members.length;
  }

  /** The g-th group's rows (positions, ascending; a copy). */
  public int[] members(int g) {
    return members[g].clone();
  }

  /** The g-th group's ids (positions when there are no ids). */
  public long[] ids(int g) {
    long[] r = new long[members[g].length];
    for (int i = 0; i < r.length; i++) r[i] = ids != null ? ids[members[g][i]] : members[g][i];
    return r;
  }

  /** JSON: [[3, 9, 12], [5, 7]] (ids). */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("[");
    for (int g = 0; g < members.length; g++) {
      if (g > 0) b.append(", ");
      b.append('[');
      long[] x = ids(g);
      for (int i = 0; i < x.length; i++) b.append(i > 0 ? ", " : "").append(x[i]);
      b.append(']');
    }
    return b.append(']').toString();
  }
}
