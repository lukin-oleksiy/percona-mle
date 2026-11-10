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

/**
 * A stored function or procedure to call (Oracle's stored routine API: Schema.getFunction and
 * getProcedure). A function call is SELECT f(?, ...) and returns the value; a procedure call is
 * CALL p(...), its result sets are dropped, and its OUT and INOUT arguments are {@link Argument}s
 * (passed through the user variables @_mle_o1, ...).
 */
public final class Routine {
  private final Session session;
  private final String schema;
  private final String name;
  private final boolean procedure;
  private boolean closed;

  Routine(Session session, String schema, String name, boolean procedure) {
    this.session = session;
    this.schema = schema;
    this.name = name;
    this.procedure = procedure;
  }

  public boolean isProcedure() {
    return procedure;
  }

  public String getName() {
    return name;
  }

  public Object call(Object... args) {
    if (closed) throw new IllegalStateException((procedure ? "procedure " : "function ") + name + " was closed");
    String qualified = Session.quoteName(schema) + "." + Session.quoteName(name);
    if (!procedure) {
      StringBuilder q = new StringBuilder("SELECT ").append(qualified).append('(');
      for (int i = 0; i < args.length; i++) q.append(i == 0 ? "?" : ", ?");
      SqlResult r = session.run(q.append(')').toString(), args, session.effective(null));
      Object v = r.fetchOne().get(0);
      r.close();
      return v;
    }
    StringBuilder q = new StringBuilder("CALL ").append(qualified).append('(');
    List<Object> params = new ArrayList<>();
    List<Argument> outs = new ArrayList<>();
    StringBuilder set = new StringBuilder("SET ");
    StringBuilder get = new StringBuilder("SELECT ");
    List<Object> initial = new ArrayList<>();
    for (int i = 0; i < args.length; i++) {
      if (i > 0) q.append(", ");
      if (args[i] instanceof Argument a) {
        String var = "@_mle_o" + (outs.size() + 1);
        q.append(var);
        set.append(outs.isEmpty() ? "" : ", ").append(var).append(" = ?");
        get.append(outs.isEmpty() ? "" : ", ").append(var);
        initial.add(a.getVal());
        outs.add(a);
      } else {
        q.append('?');
        params.add(args[i]);
      }
    }
    // The procedure reads INOUT values when the CALL starts and writes OUT values when it ends,
    // so nested calls using the same variables meanwhile do not matter.
    if (!outs.isEmpty()) session.exec(set.toString(), initial.toArray());
    SqlResult r = session.run(q.append(')').toString(), params.toArray(), session.effective(null));
    if (r != null) r.close();
    if (!outs.isEmpty()) {
      Row row = session.run(get.toString(), null, session.effective(null)).fetchOne();
      for (int i = 0; i < outs.size(); i++) outs.get(i).setVal(row.get(i));
    }
    return null;
  }

  public void close() {
    if (closed) throw new IllegalStateException((procedure ? "procedure " : "function ") + name + " was closed");
    closed = true;
  }
}
