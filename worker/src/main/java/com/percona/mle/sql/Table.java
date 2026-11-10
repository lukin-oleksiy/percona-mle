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

/** A table or view (Oracle's Table object). */
public final class Table {
  private final Session session;
  private final Schema schema;
  private final String name;

  Table(Session session, Schema schema, String name) {
    this.session = session;
    this.schema = schema;
    this.name = name;
  }

  public String getName() {
    return name;
  }

  public Schema getSchema() {
    return schema;
  }

  public Session getSession() {
    return session;
  }

  public boolean existsInDatabase() {
    return !session.column("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?",
        schema.getName(), name).isEmpty();
  }

  public boolean isView() {
    return !session.column("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? "
        + "AND TABLE_TYPE = 'VIEW'", schema.getName(), name).isEmpty();
  }

  /** The number of rows (an error if the table does not exist). */
  public long count() {
    Object n = session.column("SELECT COUNT(*) FROM " + Session.quoteName(schema.getName()) + "."
        + Session.quoteName(name)).get(0);
    return ((Number) n).longValue();
  }

  @Override
  public String toString() {
    return schema.getName() + "." + name;
  }
}
