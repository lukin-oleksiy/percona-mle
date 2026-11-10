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

import java.util.List;

/**
 * A row of a {@link SqlResult} (Oracle's Row): values by index or by column label, in the
 * language of the session that ran the statement (mapped when first read).
 */
public final class Row {
  private static final Object UNMAPPED = new Object();

  private final SqlResult.Set set;
  private final Object[] raw;
  private Object[] mapped;

  Row(SqlResult.Set set, Object[] raw) {
    this.set = set;
    this.raw = raw;
  }

  /** The value of column i (from 0). */
  public Object get(int i) {
    if (i < 0 || i >= raw.length) throw new IndexOutOfBoundsException("no column " + i + " in a row of " + raw.length);
    if (mapped == null) {
      mapped = new Object[raw.length];
      java.util.Arrays.fill(mapped, UNMAPPED);
    }
    Object v = mapped[i];
    if (v == UNMAPPED) {
      v = raw[i] == null ? null : set.mapper.value(raw[i], set.columns.get(i), set.options);
      mapped[i] = v;
    }
    return v;
  }

  /** The value of the first column labelled name. */
  public Object getField(String name) {
    Integer i = set.index.get(name);
    if (i == null) throw new IllegalArgumentException("no column '" + name + "' in the row");
    return get(i);
  }

  /** The index of the first column labelled name, or -1. */
  public int indexOf(String name) {
    Integer i = set.index.get(name);
    return i == null ? -1 : i;
  }

  public int getLength() {
    return raw.length;
  }

  public Object[] toArray() {
    Object[] a = new Object[raw.length];
    for (int i = 0; i < a.length; i++) a[i] = get(i);
    return a;
  }

  public List<Column> getColumns() {
    return set.columns;
  }

  /** The labels of the row's columns, without repeats (first occurrence). */
  public List<String> getLabels() {
    return set.labels;
  }

  @Override
  public String toString() {
    return java.util.Arrays.toString(toArray());
  }
}
