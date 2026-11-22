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

import java.util.HashMap;
import java.util.Map;

/**
 * The states of the SQL statements of one request (protocol §10.1), kept by the worker for every
 * language: the SQL API of each (Java's, JavaScript's, Python's) runs its statements through the
 * one tracker of the routine call it serves.
 *
 * <p>A statement id is CLOSED (not in use), IDLE (prepared, no result to read), ROWS (a result
 * set has rows to fetch) or NEXT (the current result is complete and another one follows). An id
 * opened by PREPARE is a prepared statement and its home is IDLE; one opened by EXECUTE is a
 * direct statement and its home is CLOSED. Only ids that are not CLOSED are kept.
 *
 * <p>One round trip is an {@link Exchange}: {@link #begin} before the SQL message is sent, then
 * exactly one of error, ok, rows or end for the reply, which moves the id to its next state. Of a
 * reply the worker checks only its kind ({@link Exchange#accepts}); the rest is the host's to get
 * right (§1).
 *
 * <p>The tracker also keeps the values of a prepared statement's last execution: the host takes
 * all values with every execution, so the API's partial re-binding is done here ({@link #bind}).
 */
public final class Statements {
  // The actions of an SQL message (§10.2).
  public static final int EXECUTE = 0;
  public static final int PREPARE = 1;
  public static final int EXECUTE_PREPARED = 2;
  public static final int FETCH = 3;
  public static final int NEXT_RESULT = 4;
  public static final int CLOSE = 5;
  public static final int EXECUTE_MANY = 6;
  /** The flag PASS_RESULT_TO_CLIENT: given at EXECUTE or PREPARE, it holds for the life of the id. */
  public static final int TO_CLIENT = 1;

  // The kinds of an SQL_RESULT (§9.5).
  public static final int ERROR = 0;
  public static final int OK = 1;
  public static final int ROWS = 2;
  public static final int END = 3;

  public enum State { CLOSED, IDLE, ROWS, NEXT }

  private static final class Statement {
    final boolean prepared;
    final boolean toClient;
    State state;
    /** The current result set's columns, as the decoder keeps them. */
    Object columns;
    /**
     * A prepared statement: the columns received last for the id. A later result set of the id
     * that has exactly these columns comes without them (§10.3).
     */
    Object lastColumns;
    /** The values of the last execution of a prepared statement. */
    Object[] bound;

    Statement(boolean prepared, boolean toClient, State state) {
      this.prepared = prepared;
      this.toClient = toClient;
      this.state = state;
    }
  }

  private final Map<Long, Statement> open = new HashMap<>();

  /** A new request starts with no ids (§10.1). */
  public void clear() {
    open.clear();
  }

  public State state(long id) {
    Statement s = open.get(id);
    return s == null ? State.CLOSED : s.state;
  }

  /** The statements that are not CLOSED. */
  public int open() {
    return open.size();
  }

  /**
   * The values to send with EXECUTE_PREPARED of id when the routine gave params: they bind the
   * leading markers, and the markers after them keep the values of the last execution.
   */
  public Object[] bind(long id, Object[] params) {
    Statement s = open.get(id);
    if (s == null || !s.prepared) return params;
    Object[] given = params == null ? new Object[0] : params;
    if (s.bound != null && given.length < s.bound.length) {
      Object[] all = s.bound.clone();
      System.arraycopy(given, 0, all, 0, given.length);
      given = all;
    }
    s.bound = given;
    return given.length == 0 ? null : given;
  }

  /** An SQL message is about to be sent: action on id (flags: of EXECUTE and PREPARE). */
  public Exchange begin(int action, long id, int flags) {
    return new Exchange(action, id, (flags & TO_CLIENT) != 0);
  }

  /** One SQL message and its SQL_RESULT. */
  public final class Exchange {
    private final int action;
    private final long id;
    private final Statement s;
    private final boolean allowed;
    private final boolean toClient;

    private Exchange(int action, long id, boolean toClientFlag) {
      this.action = action;
      this.id = id;
      this.s = open.get(id);
      this.allowed = switch (action) {
        case EXECUTE, PREPARE -> s == null;
        case EXECUTE_PREPARED, EXECUTE_MANY -> s != null && s.prepared;
        case FETCH -> s != null && s.state == State.ROWS;
        default -> true;  // NEXT_RESULT, CLOSE
      };
      this.toClient = s != null ? s.toClient : toClientFlag;
    }

    /** Whether the action is allowed in the id's state; one that is not is answered ERROR (rule 2). */
    public boolean allowed() {
      return allowed;
    }

    /** Whether a reply of this kind may answer the action (the table of §10.1). */
    public boolean accepts(int kind) {
      if (action == CLOSE) return kind == OK;
      if (!allowed) return kind == ERROR;
      if (kind == ERROR) return true;
      // The results of an id with PASS_RESULT_TO_CLIENT do not come to the worker (§10.5).
      if (kind == ROWS && toClient) return false;
      return switch (action) {
        case EXECUTE, EXECUTE_PREPARED -> kind == OK || kind == ROWS;
        case PREPARE, EXECUTE_MANY -> kind == OK;
        case FETCH -> kind == ROWS;
        // In IDLE and CLOSED there is nothing to move to.
        case NEXT_RESULT -> s == null || s.state == State.IDLE ? kind == END : kind == OK || kind == ROWS || kind == END;
        default -> false;
      };
    }

    /** Whether a ROWS reply is the first batch of its result set (it carries the columns). */
    public boolean firstBatch() {
      return action != FETCH;
    }

    /**
     * The columns of a batch that carries none: of a later batch, the ones its set's first batch
     * had; of a first batch, which only a prepared id can have without columns, the ones received
     * last for the id. Null if there are none.
     */
    public Object columns() {
      if (s == null) return null;
      return firstBatch() ? s.lastColumns : s.columns;
    }

    private void home(Statement t) {
      if (t == null) return;
      t.columns = null;
      if (t.prepared) t.state = State.IDLE;
      else open.remove(id);
    }

    /** The reply is ERROR: an allowed action sends the id home (rule 3), another changes nothing. */
    public void error() {
      if (allowed) home(s);
    }

    /** The reply is OK. */
    public void ok(boolean moreResults, Object[] lastRow) {
      switch (action) {
        case CLOSE -> open.remove(id);
        case PREPARE -> open.put(id, new Statement(true, toClient, State.IDLE));
        case EXECUTE -> {
          if (moreResults) open.put(id, new Statement(false, toClient, State.NEXT));
        }
        default -> {  // EXECUTE_PREPARED, EXECUTE_MANY, NEXT_RESULT on an open id
          if (action == EXECUTE_MANY && lastRow != null) s.bound = lastRow;
          if (moreResults) {
            s.columns = null;
            s.state = State.NEXT;
          } else {
            home(s);
          }
        }
      }
    }

    /** The reply is ROWS: one batch of the set whose columns, as the decoder keeps them, are columns. */
    public void rows(Object columns, boolean more, boolean moreResults) {
      Statement t = s;
      if (t != null && t.prepared) t.lastColumns = columns;
      if (more) {
        if (t == null) {  // EXECUTE: a direct statement with rows to fetch
          t = new Statement(false, toClient, State.ROWS);
          open.put(id, t);
        }
        t.state = State.ROWS;
        t.columns = columns;
      } else if (moreResults) {
        if (t == null) {
          t = new Statement(false, toClient, State.NEXT);
          open.put(id, t);
        }
        t.columns = null;
        t.state = State.NEXT;
      } else {
        home(t);
      }
    }

    /** The reply is END (to NEXT_RESULT): there is no further result. */
    public void end() {
      home(s);
    }

    /** The id is closed without a reply to go by: the worker closed it after a reply it could not hold. */
    public void closed() {
      open.remove(id);
    }
  }
}
