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

package com.percona.mle.worker;


/**
 * Entry from {@link com.percona.mle.api.Mle} into the worker. Public only so that the API package
 * can reach it; routine class loaders do not see the worker package.
 */
public final class SqlBridge {
  private SqlBridge() {}

  static volatile Dispatcher dispatcher;

  private static com.percona.mle.sql.Session javaSession;

  /** Mle.session(): the Java routines' session (Java values). */
  public static synchronized com.percona.mle.sql.Session javaSession() {
    dispatcher();
    if (javaSession == null) javaSession = new com.percona.mle.sql.Session(new JavaSql());
    return javaSession;
  }

  /** MleSql: one Phase 5 SQL request (see Dispatcher#sqlRequest). */
  public static com.percona.mle.api.MleSql.Reply request(int action, long id, String text, Object[] params,
      int flags, int maxRows) {
    Dispatcher d = dispatcher;
    if (d == null) throw new IllegalStateException("MLE worker not running");
    return d.sqlRequest(action, id, text, params, flags, maxRows);
  }

  /**
   * com.percona.mle.sql: a request whose rows keep raw values (see Dispatcher#sqlRequest);
   * vectorBuffers: VECTORs as ByteBuffers, not float[]; sink: the rows go to it, not into the reply.
   */
  public static com.percona.mle.api.MleSql.Reply requestRaw(int action, long id, String text, Object[] params,
      int flags, int maxRows, boolean vectorBuffers, com.percona.mle.sql.RowSink sink) {
    return dispatcher().sqlRequest(action, id, text, params, flags, maxRows,
        vectorBuffers ? Dispatcher.RAW_BUFFERS : Dispatcher.RAW, sink);
  }

  /** EXECUTE_MANY of prepared statement id: params are rows of rowWidth values (exp note 51). */
  public static com.percona.mle.api.MleSql.Reply requestMany(long id, Object[] params, int rowWidth) {
    return dispatcher().sqlRequest(Wire.SQL_EXECUTE_MANY, id, null, params, 0, 0, Dispatcher.RAW, null, rowWidth);
  }

  /** The values one SQL message may carry. */
  public static final int MAX_VALUES = Wire.MAX_VALUES;

  /** The running routine's schema (the current database while it runs), or null. */
  public static String routineSchema() {
    Dispatcher d = dispatcher;
    return d == null ? null : d.routineSchema();
  }

  /** The bytes of parameters a batch message may carry (one mailbox arena, with room to spare). */
  public static int batchBytes() {
    return dispatcher().arenaSize() / 4 * 3;
  }

  /** Statement id's unread rows are drained before another statement runs (Dispatcher#deferDrain). */
  public static void deferDrain(long id, Runnable drain) {
    dispatcher().deferDrain(id, drain);
  }

  /** Statement id's rows need no draining any more. */
  public static void drained(long id) {
    Dispatcher d = dispatcher;
    if (d != null) d.drained(id);
  }

  /** The running top-level call's sequence number. */
  public static long callSeq() {
    return dispatcher().callSeq();
  }

  /** The running routine call (a nested routine's session options are its own): unique per call. */
  public static long callScope() {
    return dispatcher().callScope();
  }

  /** The running routine call's nesting depth (0: called from the client's statement). */
  public static int callDepth() {
    return dispatcher().depth();
  }

  /** Whether the running call was killed. */
  public static boolean interrupted() {
    return dispatcher().interrupted();
  }

  /** Whether the running call was killed; false outside a worker (the vector helper in Train). */
  public static boolean killed() {
    Dispatcher d = dispatcher;
    return d != null && d.interrupted();
  }

  private static Dispatcher dispatcher() {
    Dispatcher d = dispatcher;
    if (d == null) throw new IllegalStateException("MLE worker not running");
    return d;
  }

  public static long newStatementId() {
    Dispatcher d = dispatcher;
    if (d == null) throw new IllegalStateException("MLE worker not running");
    return d.newStatementId();
  }

  public static org.graalvm.polyglot.Value importPython(String module) {
    return dispatcher().polyglot().importPython(module);
  }

  public static org.graalvm.polyglot.Value importJs(String name) {
    return dispatcher().polyglot().importJs(name);
  }

  /** Mle.importLibrary: "schema.name" or "/schema/name", as Python's import_library. */
  public static org.graalvm.polyglot.Value importLibrary(String library) {
    String p = library.replaceAll("^/+|/+$", "").replace('/', '.');
    int dot = p.indexOf('.');
    if (dot <= 0 || dot == p.length() - 1 || p.indexOf('.', dot + 1) >= 0)
      throw new IllegalArgumentException("a library is 'schema.name' or '/schema/name', not '" + library + "'");
    Polyglot polyglot = dispatcher().polyglot();
    return polyglot.context().asValue(polyglot.libraryModule(p.substring(0, dot), p.substring(dot + 1), false));
  }

  public static org.graalvm.polyglot.Context context() {
    Dispatcher d = dispatcher;
    if (d == null) throw new IllegalStateException("MLE worker not running");
    return d.polyglot().context();
  }
}
