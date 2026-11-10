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

import com.percona.mle.api.MleSql;
import com.percona.mle.api.MleSqlException;
import java.util.ArrayList;
import java.util.List;

/**
 * A prepared statement (Oracle's PreparedStatement), from {@link Session#prepare}. Values given
 * to {@link #bind} bind the markers in order at the next {@link #execute()}; the first execution
 * needs all of them, later ones keep the values of markers not bound again.
 */
public final class PreparedStatement {
  private final Session session;
  private final long id;
  private final SqlOptions options;
  private final int parameterCount;
  private final List<Object> pending = new ArrayList<>();
  private boolean executed;
  private boolean deallocated;
  private SqlResult last;

  PreparedStatement(Session session, long id, SqlOptions options, int parameterCount) {
    this.session = session;
    this.id = id;
    this.options = options;
    this.parameterCount = parameterCount;
  }

  private void open() {
    if (deallocated) throw new IllegalStateException("the prepared statement was deallocated");
  }

  public PreparedStatement bind(Object... values) {
    open();
    if (pending.size() + values.length > parameterCount)
      throw new MleSqlException(1210, "HY000", "Incorrect arguments to bind: " + (pending.size() + values.length)
          + " values for " + parameterCount + " parameter markers");
    for (Object v : values) pending.add(v);
    return this;
  }

  /** Executes with the values bound since the last execution; null with passResultToClient. */
  public SqlResult execute() {
    open();
    if (!executed && pending.size() < parameterCount)
      throw new MleSqlException(1210, "HY000", "Incorrect arguments to EXECUTE: " + pending.size() + " of "
          + parameterCount + " parameter markers bound");
    if (last != null) last.replace();
    last = null;
    Object[] params = session.params(pending.toArray());
    pending.clear();
    MleSql.Reply r = session.request(Session.EXECUTE_PREPARED, id, null, params, 0, 0);
    executed = true;
    if (options.passResultToClient()) return null;
    last = new SqlResult(session, id, false, options, r);
    return last;
  }

  /** Closes the statement in mysqld. */
  public void deallocate() {
    if (deallocated) return;
    deallocated = true;
    if (last != null) last.replace();
    session.request(Session.CLOSE, id, null, null, 0, 0);
  }

  public int getParameterCount() {
    return parameterCount;
  }

  public Object getOption(String name) {
    return options.get(name);
  }
}
