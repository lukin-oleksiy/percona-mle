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

import java.util.ArrayList;
import java.util.List;

/** A database (Oracle's Schema object). */
public final class Schema {
  private final Session session;
  private final String name;

  Schema(Session session, String name) {
    this.session = session;
    this.name = name;
  }

  public String getName() {
    return name;
  }

  public Session getSession() {
    return session;
  }

  public boolean existsInDatabase() {
    return session.schemaExists(name);
  }

  /** The table or view name (whether it exists is not checked). */
  public Table getTable(String table) {
    return new Table(session, this, table);
  }

  /** The schema's tables and views, by name. */
  public List<Table> getTables() {
    List<Table> list = new ArrayList<>();
    for (Object t : session.column(
        "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? ORDER BY TABLE_NAME", name))
      list.add(new Table(session, this, (String) t));
    return list;
  }

  /** A stored function of this schema, to call (Oracle's stored routine API). */
  public Routine getFunction(String function) {
    return session.routine(name, function, false);
  }

  /** A stored procedure of this schema, to call; OUT and INOUT arguments are {@link Argument}s. */
  public Routine getProcedure(String procedure) {
    return session.routine(name, procedure, true);
  }

  @Override
  public String toString() {
    return name;
  }
}
