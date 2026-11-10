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
import com.percona.mle.worker.SqlBridge;
import java.util.ArrayList;
import java.util.List;

/**
 * SQL from a routine (Oracle's Session object; the JavaScript global {@code session}): statements
 * run in mysqld in the calling session and transaction, with the routine's privileges, and only
 * in stored procedures (functions and triggers: error 6120). Methods must be called on the
 * routine's thread, during a call.
 *
 * <p>One Session per language: its {@link ValueMapper} decides how values look. Options set with
 * {@link #setOptions} hold for the rest of the top-level routine call.
 */
public final class Session {
  static final int EXECUTE = 0;
  static final int PREPARE = 1;
  static final int EXECUTE_PREPARED = 2;
  static final int FETCH = 3;
  static final int NEXT_RESULT = 4;
  static final int CLOSE = 5;
  static final int TO_CLIENT = 1;

  private final ValueMapper mapper;
  private SqlOptions callOptions;
  private long callOptionsSeq = -1;
  private long savepoints;

  public Session(ValueMapper mapper) {
    this.mapper = mapper;
  }

  ValueMapper mapper() {
    return mapper;
  }

  MleSql.Reply request(int action, long id, String text, Object[] params, int flags, int maxRows) {
    return SqlBridge.requestRaw(action, id, text, params, flags, maxRows);
  }

  /** The options of a statement: o over this call's session options over the defaults. */
  SqlOptions effective(SqlOptions o) {
    SqlOptions base = callOptionsSeq == SqlBridge.callSeq() ? callOptions : null;
    SqlOptions merged = base == null ? mapper instanceof Defaults d ? d.defaults() : new SqlOptions()
        : base;
    return o == null ? merged.over(null) : o.over(merged);
  }

  /** A mapper with default options (the session state's integer and decimal types). */
  public interface Defaults {
    SqlOptions defaults();
  }

  Object[] params(Object[] values) {
    if (values == null || values.length == 0) return null;
    Object[] p = new Object[values.length];
    for (int i = 0; i < p.length; i++) p[i] = mapper.param(values[i]);
    return p;
  }

  // ------------------------------------------------------------ statements

  /** A simple statement (no parameter markers), run with {@link SqlExecute#execute()}. */
  public SqlExecute sql(String sql) {
    return sql(sql, null);
  }

  public SqlExecute sql(String sql, SqlOptions options) {
    return new SqlExecute(this, sql, effective(options));
  }

  /**
   * Runs a statement; with parameters as a one-shot prepared statement. Null with
   * passResultToClient (the results went to the client).
   */
  public SqlResult runSql(String sql, Object... params) {
    return run(sql, params, effective(null));
  }

  public SqlResult runSql(String sql, Object[] params, SqlOptions options) {
    return run(sql, params, effective(options));
  }

  SqlResult run(String sql, Object[] params, SqlOptions o) {
    long id = SqlBridge.newStatementId();
    MleSql.Reply r = request(EXECUTE, id, sql, params(params), o.passResultToClient() ? TO_CLIENT : 0, 0);
    return o.passResultToClient() ? null : new SqlResult(this, id, true, o, r);
  }

  /** Runs a statement for its effect; its results are dropped. */
  void exec(String sql, Object... params) {
    long id = SqlBridge.newStatementId();
    MleSql.Reply r = request(EXECUTE, id, sql, params(params), 0, 0);
    if (r.moreRows() || r.moreResults()) request(CLOSE, id, null, null, 0, 0);
  }

  /** The raw first column of each row of an internal query (catalog lookups). */
  List<Object> column(String sql, Object... params) {
    long id = SqlBridge.newStatementId();
    MleSql.Reply r = request(EXECUTE, id, sql, params, 0, 0);
    List<Object> values = new ArrayList<>();
    while (true) {
      for (Object[] row : r.rows()) values.add(row[0]);
      if (!r.moreRows()) break;
      r = request(FETCH, id, null, null, 0, 0);
    }
    if (r.moreResults()) request(CLOSE, id, null, null, 0, 0);
    return values;
  }

  public PreparedStatement prepare(String sql) {
    return prepare(sql, null);
  }

  public PreparedStatement prepare(String sql, SqlOptions options) {
    SqlOptions o = effective(options);
    long id = SqlBridge.newStatementId();
    MleSql.Reply r = request(PREPARE, id, sql, null, o.passResultToClient() ? TO_CLIENT : 0, 0);
    return new PreparedStatement(this, id, o, r.parameterCount());
  }

  /** Sets defaults for the statements of the rest of this routine call (unset ones are kept). */
  public void setOptions(SqlOptions options) {
    SqlOptions base = callOptionsSeq == SqlBridge.callSeq() ? callOptions : null;
    callOptions = options.over(base);
    callOptionsSeq = SqlBridge.callSeq();
  }

  public Object getOption(String name) {
    return effective(null).get(name);
  }

  // ------------------------------------------------------------ schemas

  /** name quoted as an identifier: `name`, with backticks doubled. */
  public static String quoteName(String name) {
    return "`" + name.replace("`", "``") + "`";
  }

  boolean schemaExists(String name) {
    return !column("SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?", name).isEmpty();
  }

  /** The schema; error 1049 if it does not exist. */
  public Schema getSchema(String name) {
    if (!schemaExists(name)) throw new MleSqlException(1049, "42000", "Unknown database '" + name + "'");
    return new Schema(this, name);
  }

  public List<Schema> getSchemas() {
    List<Schema> list = new ArrayList<>();
    for (Object n : column("SELECT SCHEMA_NAME FROM information_schema.SCHEMATA ORDER BY SCHEMA_NAME"))
      list.add(new Schema(this, (String) n));
    return list;
  }

  /** The current database (in a routine: the routine's), or null if none. */
  public Schema getDefaultSchema() {
    Object n = column("SELECT DATABASE()").get(0);
    return n == null ? null : new Schema(this, (String) n);
  }

  public Schema createSchema(String name) {
    exec("CREATE DATABASE " + quoteName(name));
    return new Schema(this, name);
  }

  public void dropSchema(String name) {
    exec("DROP DATABASE " + quoteName(name));
  }

  /** A stored function or procedure of schema; error 1305 if it does not exist. */
  Routine routine(String schema, String name, boolean procedure) {
    String type = procedure ? "PROCEDURE" : "FUNCTION";
    if (column("SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = ? AND ROUTINE_NAME = ? "
        + "AND ROUTINE_TYPE = ?", schema, name, type).isEmpty())
      throw new MleSqlException(1305, "42000", type + " " + schema + "." + name + " does not exist");
    return new Routine(this, schema, name, procedure);
  }

  // ------------------------------------------------------------ transactions

  public void startTransaction() {
    exec("START TRANSACTION");
  }

  public void commit() {
    exec("COMMIT");
  }

  public void rollback() {
    exec("ROLLBACK");
  }

  /** A savepoint with a generated name (TxSp1, TxSp2, ...); returns the name. */
  public String setSavepoint() {
    return setSavepoint("TxSp" + ++savepoints);
  }

  public String setSavepoint(String name) {
    exec("SAVEPOINT " + quoteName(savepointName(name)));
    return name;
  }

  public void rollbackTo(String name) {
    exec("ROLLBACK TO SAVEPOINT " + quoteName(savepointName(name)));
  }

  public void releaseSavepoint(String name) {
    exec("RELEASE SAVEPOINT " + quoteName(savepointName(name)));
  }

  private static String savepointName(String name) {
    if (name == null || name.isEmpty()) throw new IllegalArgumentException("the savepoint name must not be empty");
    return name;
  }

  /** The autocommit system variable (1 or 0). */
  public long autocommit() {
    return ((Number) column("SELECT @@autocommit").get(0)).longValue();
  }

  public void autocommit(boolean on) {
    exec(on ? "SET autocommit = 1" : "SET autocommit = 0");
  }

  // ------------------------------------------------------------ user variables

  /** The user variable @name (null if not set). */
  public Object getVariable(String name) {
    SqlResult r = run("SELECT @" + quoteName(name), null, effective(null));
    return r.fetchOne().get(0);
  }

  /** Sets the user variable @name (creates it if needed). */
  public void setVariable(String name, Object value) {
    long id = SqlBridge.newStatementId();
    request(EXECUTE, id, "SET @" + quoteName(name) + " = ?", new Object[] {mapper.param(value)}, 0, 0);
  }
}
