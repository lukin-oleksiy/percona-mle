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

/** A simple statement (Oracle's SqlExecute), from {@link Session#sql}. */
public final class SqlExecute {
  private final Session session;
  private final String sql;
  private final SqlOptions options;

  SqlExecute(Session session, String sql, SqlOptions options) {
    this.session = session;
    this.sql = sql;
    this.options = options;
  }

  /** Runs the statement; null with passResultToClient. */
  public SqlResult execute() {
    return session.run(sql, null, options);
  }

  public Object getOption(String name) {
    return options.get(name);
  }
}
