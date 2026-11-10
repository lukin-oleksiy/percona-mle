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

import com.percona.mle.api.MleSqlException;
import com.percona.mle.sql.Argument;
import com.percona.mle.sql.Column;
import com.percona.mle.sql.PreparedStatement;
import com.percona.mle.sql.Routine;
import com.percona.mle.sql.Row;
import com.percona.mle.sql.Schema;
import com.percona.mle.sql.Session;
import com.percona.mle.sql.SqlExecute;
import com.percona.mle.sql.SqlOptions;
import com.percona.mle.sql.SqlResult;
import com.percona.mle.sql.Table;
import com.percona.mle.sql.ValueMapper;
import com.percona.mle.sql.Warning;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.function.Supplier;
import org.graalvm.polyglot.PolyglotException;
import org.graalvm.polyglot.Value;
import org.graalvm.polyglot.proxy.ProxyArray;
import org.graalvm.polyglot.proxy.ProxyExecutable;
import org.graalvm.polyglot.proxy.ProxyObject;

/**
 * The guest languages' view of the SQL API (com.percona.mle.sql, 12 §13): polyglot proxies over
 * the Java core, one implementation for JavaScript and Python. A {@link Lang} supplies what
 * differs: values (through its ValueMapper), lists, errors, options objects and method names.
 *
 * <p>Rows are ProxyObject + ProxyArray: row.col, row['col'] (JS), row[i], with values mapped
 * when first read (S-12: 12 ns per row in JS, 47 in Python).
 */
final class GuestSql {
  private GuestSql() {}

  /** What a guest language supplies. */
  interface Lang extends ValueMapper, Session.Defaults {
    /** A guest array (JS) or list (Python) of these elements (already guest values). */
    Object list(List<?> elements);

    /** The guest's SqlError for e, to throw. */
    RuntimeException sqlError(MleSqlException e);

    /** A guest error for another failure (bad arguments, a closed object ...), to throw. */
    RuntimeException error(RuntimeException e);

    /** The API's (camelCase) name of a guest member name (Python: snake_case aliases). */
    String member(String name);

    /** A guest options object as SqlOptions; null for null/undefined. */
    SqlOptions options(Value v);

    /** The guest's null (JS null; Python None, not a foreign null). */
    default Object none() {
      return null;
    }

    /** v, or the guest's null. */
    default Object out(Object v) {
      return v == null ? none() : v;
    }
  }

  // ------------------------------------------------------------ base

  /** An object with methods: members are ProxyExecutables, errors become guest errors. */
  abstract static class Methods implements ProxyObject {
    final Lang lang;

    Methods(Lang lang) {
      this.lang = lang;
    }

    abstract Set<String> names();

    /** Calls method name (one of names()) with the guest arguments. */
    abstract Object call(String name, Value[] a);

    @Override
    public Object getMember(String key) {
      String k = lang.member(key);
      if (!names().contains(k)) return other(key);
      return (ProxyExecutable) a -> lang.out(guard(lang, () -> call(k, a)));
    }

    /** A member that is not a method (session variables); null if none. */
    Object other(String key) {
      return null;
    }

    @Override
    public Object getMemberKeys() {
      return ProxyArray.fromList(new ArrayList<>(names()));
    }

    @Override
    public boolean hasMember(String key) {
      return names().contains(lang.member(key));
    }

    @Override
    public void putMember(String key, Value value) {
      throw new UnsupportedOperationException("cannot set " + key);
    }
  }

  static Object guard(Lang lang, Supplier<Object> f) {
    try {
      return f.get();
    } catch (MleSqlException e) {
      throw lang.sqlError(e);
    } catch (PolyglotException e) {
      throw e;  // a guest error (or a KILL) passing through: unchanged
    } catch (RuntimeException e) {
      throw lang.error(e);
    }
  }

  static Value arg(Value[] a, int i) {
    return i < a.length && !a[i].isNull() ? a[i] : null;
  }

  static String str(Value[] a, int i) {
    Value v = arg(a, i);
    if (v == null) throw new IllegalArgumentException("argument " + (i + 1) + " is missing");
    return v.isString() ? v.asString() : v.toString();
  }

  static Object[] values(Value[] a, int from) {
    Object[] v = new Object[Math.max(0, a.length - from)];
    for (int i = 0; i < v.length; i++) v[i] = a[from + i];
    return v;
  }

  /** The elements of a guest array (or one value as a single parameter). */
  static Object[] elements(Value v) {
    if (v == null) return new Object[0];
    if (!v.hasArrayElements()) return new Object[] {v};
    Object[] p = new Object[Math.toIntExact(v.getArraySize())];
    for (int i = 0; i < p.length; i++) p[i] = v.getArrayElement(i);
    return p;
  }

  static boolean truthy(Value v) {
    if (v == null) return false;
    if (v.isBoolean()) return v.asBoolean();
    if (v.isNumber()) return v.fitsInDouble() && v.asDouble() != 0;
    if (v.isString()) return !v.asString().isEmpty();
    return true;
  }

  // ------------------------------------------------------------ session

  /** The session object: its methods, and the user variables as other members (x ↔ @x). */
  static final class GSession extends Methods {
    private static final Set<String> NAMES = Set.of("sql", "runSql", "prepare", "setOptions", "getOption",
        "getSchema", "getSchemas", "getDefaultSchema", "createSchema", "dropSchema", "quoteName",
        "startTransaction", "commit", "rollback", "setSavepoint", "rollbackTo", "releaseSavepoint", "autocommit");
    final Session s;

    GSession(Lang lang) {
      super(lang);
      this.s = new Session(lang);
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "sql" -> new GExecute(this, s.sql(str(a, 0), lang.options(arg(a, 1))));
        case "runSql" -> {
          // runSql(query[, parameters], options)
          Value second = arg(a, 1);
          boolean params = second != null && second.hasArrayElements() && !second.isString();
          SqlResult r = s.runSql(str(a, 0), params ? elements(second) : null,
              lang.options(params ? arg(a, 2) : second));
          yield r == null ? null : new GResult(this, r);
        }
        case "prepare" -> new GPrepared(this, s.prepare(str(a, 0), lang.options(arg(a, 1))));
        case "setOptions" -> {
          SqlOptions o = lang.options(arg(a, 0));
          if (o != null) s.setOptions(o);
          yield null;
        }
        case "getOption" -> s.getOption(str(a, 0));
        case "getSchema" -> new GSchema(this, s.getSchema(str(a, 0)));
        case "getSchemas" -> {
          List<Object> l = new ArrayList<>();
          for (Schema x : s.getSchemas()) l.add(new GSchema(this, x));
          yield lang.list(l);
        }
        case "getDefaultSchema" -> {
          Schema x = s.getDefaultSchema();
          yield x == null ? null : new GSchema(this, x);
        }
        case "createSchema" -> new GSchema(this, s.createSchema(str(a, 0)));
        case "dropSchema" -> {
          s.dropSchema(str(a, 0));
          yield null;
        }
        case "quoteName" -> Session.quoteName(str(a, 0));
        case "startTransaction" -> {
          s.startTransaction();
          yield null;
        }
        case "commit" -> {
          s.commit();
          yield null;
        }
        case "rollback" -> {
          s.rollback();
          yield null;
        }
        case "setSavepoint" -> arg(a, 0) == null ? s.setSavepoint() : s.setSavepoint(str(a, 0));
        case "rollbackTo" -> {
          s.rollbackTo(arg(a, 0) == null ? "" : str(a, 0));
          yield null;
        }
        case "releaseSavepoint" -> {
          s.releaseSavepoint(arg(a, 0) == null ? "" : str(a, 0));
          yield null;
        }
        case "autocommit" -> {
          if (a.length == 0) yield s.autocommit();
          s.autocommit(truthy(arg(a, 0)));
          yield null;
        }
        default -> throw new IllegalStateException(name);
      };
    }

    /** Names starting with __ are never user variables (Python looks such names up itself). */
    private static boolean variable(String key) {
      return !key.startsWith("__");
    }

    /** session.x reads @x. */
    @Override
    Object other(String key) {
      return variable(key) ? lang.out(guard(lang, () -> s.getVariable(key))) : null;
    }

    /** Any name is a member: a user variable if not a method. */
    @Override
    public boolean hasMember(String key) {
      return variable(key) || super.hasMember(key);
    }

    /** session.x = v sets @x. */
    @Override
    public void putMember(String key, Value value) {
      if (names().contains(lang.member(key)) || !variable(key))
        throw lang.error(new IllegalArgumentException("cannot set " + key));
      guard(lang, () -> {
        s.setVariable(key, value);
        return null;
      });
    }
  }

  // ------------------------------------------------------------ statements and results

  static final class GExecute extends Methods {
    private static final Set<String> NAMES = Set.of("execute", "getOption");
    private final GSession session;
    private final SqlExecute x;

    GExecute(GSession session, SqlExecute x) {
      super(session.lang);
      this.session = session;
      this.x = x;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      if (name.equals("getOption")) return x.getOption(str(a, 0));
      SqlResult r = x.execute();
      return r == null ? null : new GResult(session, r);
    }
  }

  /** Results; also iterable over the rows of the current result set (for ... of / for ... in). */
  static final class GResult extends Methods implements org.graalvm.polyglot.proxy.ProxyIterable {
    private static final Set<String> NAMES = Set.of("fetchOne", "fetchAll", "hasData", "nextResult", "getColumns",
        "getColumnNames", "getColumnCount", "getAffectedItemsCount", "getAutoIncrementValue", "getWarnings",
        "getWarningsCount", "getExecutionTime", "close");
    private final SqlResult r;

    GResult(GSession session, SqlResult r) {
      super(session.lang);
      this.r = r;
    }

    @Override
    public Object getIterator() {
      return new org.graalvm.polyglot.proxy.ProxyIterator() {
        private Row next;

        @Override
        public boolean hasNext() {
          if (next == null) next = (Row) guard(lang, r::fetchOne);
          return next != null;
        }

        @Override
        public Object getNext() {
          if (!hasNext()) throw new java.util.NoSuchElementException();
          Row row = next;
          next = null;
          return new GRow(lang, row);
        }
      };
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "fetchOne" -> {
          Row row = r.fetchOne();
          yield row == null ? null : new GRow(lang, row);
        }
        case "fetchAll" -> {
          List<Object> l = new ArrayList<>();
          for (Row row : r.fetchAll()) l.add(new GRow(lang, row));
          yield lang.list(l);
        }
        case "hasData" -> r.hasData();
        case "nextResult" -> r.nextResult();
        case "getColumns" -> {
          List<Object> l = new ArrayList<>();
          for (Column c : r.getColumns()) l.add(new GColumn(lang, c));
          yield lang.list(l);
        }
        case "getColumnNames" -> lang.list(r.getColumnNames());
        case "getColumnCount" -> r.getColumnCount();
        case "getAffectedItemsCount" -> r.getAffectedItemsCount();
        case "getAutoIncrementValue" -> r.getAutoIncrementValue();
        case "getWarnings" -> {
          List<Object> l = new ArrayList<>();
          for (Warning w : r.getWarnings()) l.add(new GWarning(w));
          yield lang.list(l);
        }
        case "getWarningsCount" -> r.getWarningsCount();
        case "getExecutionTime" -> r.getExecutionTime();
        case "close" -> {
          r.close();
          yield null;
        }
        default -> throw new IllegalStateException(name);
      };
    }
  }

  static final class GPrepared extends Methods {
    private static final Set<String> NAMES = Set.of("bind", "execute", "deallocate", "getOption");
    private final GSession session;
    private final PreparedStatement p;

    GPrepared(GSession session, PreparedStatement p) {
      super(session.lang);
      this.session = session;
      this.p = p;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "bind" -> {
          p.bind(values(a, 0));
          yield this;
        }
        case "execute" -> {
          SqlResult r = p.execute();
          yield r == null ? null : new GResult(session, r);
        }
        case "deallocate" -> {
          p.deallocate();
          yield null;
        }
        case "getOption" -> p.getOption(str(a, 0));
        default -> throw new IllegalStateException(name);
      };
    }
  }

  /** A row: methods getField/getLength/toArray, columns by label, values by index. */
  static final class GRow implements ProxyObject, ProxyArray {
    private static final Set<String> NAMES = Set.of("getField", "getLength", "toArray");
    private final Lang lang;
    private final Row row;

    GRow(Lang lang, Row row) {
      this.lang = lang;
      this.row = row;
    }

    @Override
    public Object getMember(String key) {
      String k = lang.member(key);
      if (NAMES.contains(k)) {
        return switch (k) {
          case "getField" -> (ProxyExecutable) a -> lang.out(guard(lang, () -> row.getField(str(a, 0))));
          case "getLength" -> (ProxyExecutable) a -> row.getLength();
          default -> (ProxyExecutable) a -> guard(lang, () -> lang.list(java.util.Arrays.asList(row.toArray())));
        };
      }
      int i = row.indexOf(key);
      return i < 0 ? null : lang.out(guard(lang, () -> row.get(i)));
    }

    @Override
    public Object getMemberKeys() {
      return ProxyArray.fromList(new ArrayList<>(row.getLabels()));
    }

    @Override
    public boolean hasMember(String key) {
      return row.indexOf(key) >= 0 || NAMES.contains(lang.member(key));
    }

    @Override
    public void putMember(String key, Value value) {
      throw new UnsupportedOperationException("rows are read-only");
    }

    @Override
    public Object get(long index) {
      if (index < 0 || index >= row.getLength()) return lang.none();
      return lang.out(guard(lang, () -> row.get((int) index)));
    }

    @Override
    public void set(long index, Value value) {
      throw new UnsupportedOperationException("rows are read-only");
    }

    @Override
    public long getSize() {
      return row.getLength();
    }
  }

  static final class GColumn extends Methods {
    private static final Set<String> NAMES = Set.of("getColumnName", "getColumnLabel", "getSchemaName",
        "getTableName", "getTableLabel", "getType", "getLength", "getFractionalDigits", "getCharacterSetName",
        "getCollationName", "isNumberSigned", "isPadded");
    private final Column c;

    GColumn(Lang lang, Column c) {
      super(lang);
      this.c = c;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "getColumnName" -> c.getColumnName();
        case "getColumnLabel" -> c.getColumnLabel();
        case "getSchemaName" -> c.getSchemaName();
        case "getTableName" -> c.getTableName();
        case "getTableLabel" -> c.getTableLabel();
        case "getType" -> c.getType();
        case "getLength" -> c.getLength();
        case "getFractionalDigits" -> c.getFractionalDigits();
        case "getCharacterSetName" -> c.getCharacterSetName();
        case "getCollationName" -> c.getCollationName();
        case "isNumberSigned" -> c.isNumberSigned();
        case "isPadded" -> c.isPadded();
        default -> throw new IllegalStateException(name);
      };
    }
  }

  /** A warning: properties level, code and message (Oracle's Warning object). */
  static final class GWarning implements ProxyObject {
    private static final List<String> KEYS = List.of("level", "code", "message");
    private final Warning w;

    GWarning(Warning w) {
      this.w = w;
    }

    @Override
    public Object getMember(String key) {
      return switch (key) {
        case "level" -> w.level();
        case "code" -> w.code();
        case "message" -> w.message();
        default -> null;
      };
    }

    @Override
    public Object getMemberKeys() {
      return ProxyArray.fromList(new ArrayList<>(KEYS));
    }

    @Override
    public boolean hasMember(String key) {
      return KEYS.contains(key);
    }

    @Override
    public void putMember(String key, Value value) {
      throw new UnsupportedOperationException("warnings are read-only");
    }
  }

  // ------------------------------------------------------------ schemas, tables, routines

  static final class GSchema extends Methods {
    private static final Set<String> NAMES = Set.of("existsInDatabase", "getTable", "getTables", "getName",
        "getSession", "getFunction", "getProcedure");
    private final GSession session;
    private final Schema x;

    GSchema(GSession session, Schema x) {
      super(session.lang);
      this.session = session;
      this.x = x;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "existsInDatabase" -> x.existsInDatabase();
        case "getTable" -> new GTable(session, this, x.getTable(str(a, 0)));
        case "getTables" -> {
          List<Object> l = new ArrayList<>();
          for (Table t : x.getTables()) l.add(new GTable(session, this, t));
          yield lang.list(l);
        }
        case "getName" -> x.getName();
        case "getSession" -> session;
        case "getFunction" -> new GRoutine(lang, x.getFunction(str(a, 0)));
        case "getProcedure" -> new GRoutine(lang, x.getProcedure(str(a, 0)));
        default -> throw new IllegalStateException(name);
      };
    }
  }

  static final class GTable extends Methods {
    private static final Set<String> NAMES = Set.of("existsInDatabase", "count", "isView", "getName", "getSchema",
        "getSession");
    private final GSession session;
    private final GSchema schema;
    private final Table t;

    GTable(GSession session, GSchema schema, Table t) {
      super(session.lang);
      this.session = session;
      this.schema = schema;
      this.t = t;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      return switch (name) {
        case "existsInDatabase" -> t.existsInDatabase();
        case "count" -> t.count();
        case "isView" -> t.isView();
        case "getName" -> t.getName();
        case "getSchema" -> schema;
        case "getSession" -> session;
        default -> throw new IllegalStateException(name);
      };
    }
  }

  /** A stored routine as a guest function; f.close() releases it. */
  static final class GRoutine extends Methods implements ProxyExecutable {
    private static final Set<String> NAMES = Set.of("close");
    private final Routine r;

    GRoutine(Lang lang, Routine r) {
      super(lang);
      this.r = r;
    }

    @Override
    Set<String> names() {
      return NAMES;
    }

    @Override
    Object call(String name, Value[] a) {
      r.close();
      return null;
    }

    @Override
    public Object execute(Value... a) {
      Object[] args = new Object[a.length];
      for (int i = 0; i < a.length; i++)
        args[i] = a[i].isProxyObject() && a[i].asProxyObject() instanceof GArgument g ? g.a : a[i];
      return lang.out(guard(lang, () -> r.call(args)));
    }
  }

  /** An OUT/INOUT argument (mysql.arg()): property val. */
  static final class GArgument implements ProxyObject {
    final Argument a;
    private final Lang lang;

    GArgument(Lang lang, Value initial) {
      this.lang = lang;
      this.a = new Argument(initial == null || initial.isNull() ? null : initial);
    }

    @Override
    public Object getMember(String key) {
      return key.equals("val") ? lang.out(a.getVal()) : null;
    }

    @Override
    public Object getMemberKeys() {
      return ProxyArray.fromArray("val");
    }

    @Override
    public boolean hasMember(String key) {
      return key.equals("val");
    }

    @Override
    public void putMember(String key, Value value) {
      if (!key.equals("val")) throw new UnsupportedOperationException("an argument has only val");
      a.setVal(value);
    }
  }
}
