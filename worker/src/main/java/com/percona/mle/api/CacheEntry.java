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
 * One set of the worker's vector cache ({@link Vectors#cacheInfo}): its schema and name, kind
 * ('vectors', 'int8', 'binary'), rows and dimensions, the bytes it holds, when it was loaded, the
 * calls that found it, its idle time and when it expires, its query (null: kept by keep()), and its
 * state: 'kept', or why the last set that was not kept was not ('not kept: ... is more than
 * mle.vector_cache_max'). JSON with {@code String(e)} / {@code str(e)}.
 */
public final class CacheEntry {
  private final CacheTable.Info i;

  CacheEntry(CacheTable.Info i) {
    this.i = i;
  }

  /** The schema the name belongs to (the calling routine's). */
  public String schema() { return i.key().schema(); }

  public String name() { return i.key().name(); }

  /** 'vectors' (a VectorSet), 'int8' or 'binary' (a QuantizedSet). */
  public String kind() { return i.kind(); }

  public int rows() { return i.rows(); }

  public int dimensions() { return i.dimensions(); }

  /** The bytes the set holds (with the vectors it shares with other kept sets). */
  public long bytes() { return i.bytes(); }

  /** When it was loaded or kept (milliseconds since 1970). */
  public long loadedAt() { return i.loadedAtMs(); }

  /** The calls that found it kept. */
  public long uses() { return i.uses(); }

  /** Its idle time in milliseconds (0: never expires). */
  public long idleMs() { return i.idleMs(); }

  /** Milliseconds until it expires unless used (-1: never). */
  public long expiresInMs() { return i.expiresInMs(); }

  /** The load query (null: kept by keep()). */
  public String sql() { return i.sql(); }

  /** 'kept', or why it was not kept. */
  public String state() { return i.state(); }

  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("{\"schema\": ");
    Frame.quote(b, schema()).append(", \"name\": ");
    Frame.quote(b, name()).append(", \"kind\": \"").append(kind()).append("\", \"rows\": ").append(rows())
        .append(", \"dimensions\": ").append(dimensions()).append(", \"bytes\": ").append(bytes())
        .append(", \"uses\": ").append(uses()).append(", \"idleMs\": ").append(idleMs())
        .append(", \"expiresInMs\": ").append(expiresInMs()).append(", \"sql\": ");
    if (sql() == null) b.append("null");
    else Frame.quote(b, sql());
    b.append(", \"state\": ");
    return Frame.quote(b, state()).append('}').toString();
  }
}
