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

  /** com.percona.mle.sql: a request whose rows keep raw values (see Dispatcher#sqlRequest). */
  public static com.percona.mle.api.MleSql.Reply requestRaw(int action, long id, String text, Object[] params,
      int flags, int maxRows) {
    return dispatcher().sqlRequest(action, id, text, params, flags, maxRows, true);
  }

  /** The running top-level call's sequence number. */
  public static long callSeq() {
    return dispatcher().callSeq();
  }

  /** The running routine call: its top-level call and its nesting depth (a nested routine's
      session options are its own). */
  public static long callScope() {
    Dispatcher d = dispatcher();
    return d.callSeq() * 256 + d.depth();
  }

  /** Whether the running call was killed. */
  public static boolean interrupted() {
    return dispatcher().interrupted();
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

  public static org.graalvm.polyglot.Context context() {
    Dispatcher d = dispatcher;
    if (d == null) throw new IllegalStateException("MLE worker not running");
    return d.polyglot().context();
  }
}
