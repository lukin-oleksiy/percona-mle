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

import com.percona.mle.api.MleJson;
import com.percona.mle.api.MleSqlException;
import com.percona.mle.sql.Column;
import com.percona.mle.sql.Functions;
import com.percona.mle.sql.SqlOptions;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.util.List;
import java.util.Map;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.PolyglotException;
import org.graalvm.polyglot.Source;
import org.graalvm.polyglot.Value;
import org.graalvm.polyglot.proxy.ProxyExecutable;
import org.graalvm.polyglot.proxy.ProxyObject;

/**
 * The Python SQL API: Oracle's object model (the same Java core as JavaScript's) with snake_case
 * names (session.run_sql, result.fetch_one, row.get_field ...; the camelCase names work too) and
 * the globals session, mysql and SqlError in every Python routine.
 *
 * <p>Values: int (all integers, exact), float, decimal.Decimal (float with decimalType NUMBER),
 * str, bytes, JSON parsed (dict, list ...), datetime.date, datetime.datetime (naive, mysqld's
 * zone), datetime.timedelta (TIME), numpy.float32 arrays for VECTOR (array.array('f') without
 * numpy); NULL and a zero date are None (a Java null: "is None" holds, its type shows as
 * ForeignNone). Parameters: the same types, plus bool, set (text), list/tuple/dict
 * (JSON; not for VECTOR columns), numpy arrays and array.array (VECTOR), bytearray/memoryview (bytes).
 */
final class PySql implements GuestSql.Lang {
  private final Options options;
  private final Value list;
  private final Value bigint;
  private final Value decimal;
  private final Value bytes;
  private final Value vector;
  private final Value json;
  private final Value date;
  private final Value datetime;
  private final Value timedelta;
  private final Value kind;
  private final Value throwSql;
  private final Value throwError;

  /** Python libraries (Phase 6): a module from a body, cached per worker; a global binding. */
  final Value loadLibrary;
  final Value bind;

  private final Context context;

  PySql(Context c, Options options, ProxyExecutable library, ProxyExecutable importJs) {
    this.options = options;
    this.context = c;
    GuestSql.GSession session = new GuestSql.GSession(this);
    Map<String, Object> bridge = Map.of(
        "session", session,
        "rand", (ProxyExecutable) a -> Functions.rand(),
        "sleep", (ProxyExecutable) a -> Functions.sleep(a[0].asDouble()),
        "uuid", (ProxyExecutable) a -> Functions.uuid(),
        "isUUID", (ProxyExecutable) a -> Functions.isUUID(a[0].isNull() ? null : a[0].asString()),
        "arg", (ProxyExecutable) a -> new GuestSql.GArgument(this, a.length == 0 ? null : a[0]),
        "library", library,
        "importJs", importJs);
    Value h = c.eval(Source.create("python", PRELUDE)).execute(ProxyObject.fromMap(bridge));
    list = h.getHashValue("list");
    bigint = h.getHashValue("bigint");
    decimal = h.getHashValue("decimal");
    bytes = h.getHashValue("bytes");
    vector = h.getHashValue("vector");
    json = h.getHashValue("json");
    date = h.getHashValue("date");
    datetime = h.getHashValue("datetime");
    timedelta = h.getHashValue("timedelta");
    kind = h.getHashValue("kind");
    throwSql = h.getHashValue("throw_sql");
    throwError = h.getHashValue("throw_error");
    loadLibrary = h.getHashValue("load_library");
    bind = h.getHashValue("bind");
  }

  private static final String PRELUDE = """
      import array as _mle_array, datetime as _mle_dt, decimal as _mle_decimal, json as _mle_json
      import math as _mle_math, types as _mle_types

      class SqlError(Exception):
          \"""Oracle's SqlError(sql_state, message, error_number). Raised, MySQL raises it like
          SIGNAL; errors of SQL statements are SqlErrors too (code, sql_state, message).\"""
          def __init__(self, sql_state=None, message='', error_number=None):
              for n, v in (('sql_state', sql_state), ('error_number', error_number)):
                  if isinstance(v, float) and not _mle_math.isfinite(v):
                      raise TypeError('SqlError: ' + n + ' must be a finite number')
              if sql_state is None:
                  state = '45000'
              elif isinstance(sql_state, (int, float)) and not isinstance(sql_state, bool):
                  state = str(int(sql_state)).rjust(5, '0')
              else:
                  state = str(sql_state)
              self.message = '' if message is None else str(message)
              super().__init__(self.message)
              self.sql_state = state
              self.sqlState = state
              self.code = 1644 if error_number is None else int(error_number)
              setattr(self, '$mle$sql', True)

          def __str__(self):
              return ('SqlError: Error code: %d Error state: %s Error message: %s'
                      % (self.code, self.sql_state, self.message))

      def _mle_sql_api(B):
          types = ('TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL', 'JSON',
                   'STRING', 'BYTES', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM', 'GEOMETRY', 'VECTOR')
          ns = _mle_types.SimpleNamespace
          is_uuid = lambda v: B.isUUID(None if v is None else str(v))
          mysql = ns(
              rand=lambda: B.rand(), sleep=lambda s: B.sleep(float(s)), uuid=lambda: B.uuid(),
              is_uuid=is_uuid, isUUID=is_uuid, arg=lambda *v: B.arg(*v),
              IntegerType=ns(BIGINT='BIGINT', STRING='STRING', UNSAFE_BIGINT='UNSAFE_BIGINT',
                             UNSAFE_STRING='UNSAFE_STRING'),
              DecimalType=ns(STRING='STRING', NUMBER='NUMBER'),
              Type=ns(**{t: t for t in types}),
              SQLError=SqlError, SqlError=SqlError)
          # Database libraries (Phase 6): import_library('/db/lib') or ('db.lib') gives the
          # library's module (a Python module, or a JavaScript library's namespace).
          def import_library(path):
              p = str(path).strip('/').replace('/', '.')
              schema, _, name = p.partition('.')
              if not name:
                  raise ValueError("a library is '/schema/name' or 'schema.name', not %r" % path)
              return B.library(schema, name)

          # import_js('lodash'): npm packages (Phase 6) from Python.
          def import_js(name):
              return B.importJs(str(name))

          _mle_globals.update(session=B.session, mysql=mysql, SqlError=SqlError, import_library=import_library,
                              import_js=import_js)

          cache = {}

          def load_library(key, h, src):
              hit = cache.get(key)
              if hit is not None and hit[0] == h:
                  return hit[1]
              m = _mle_types.ModuleType(key)
              m.__dict__.update(_mle_globals)
              # Cached before it runs (as sys.modules): libraries that import each other get the
              # partly initialised module instead of recursing.
              cache[key] = (h, m)
              try:
                  exec(compile(src, key, 'exec'), m.__dict__)
              except BaseException:
                  del cache[key]
                  raise
              return m

          def bind(fn, name, value):
              # Body-form routines get fresh globals per call from a prepared dict (Phase 7).
              (fn.__mle_base__ if hasattr(fn, '__mle_base__') else fn.__globals__)[name] = value

          def throw_sql(code, state, message):
              raise SqlError(state, message, code)

          def throw_error(message, type_error):
              raise (TypeError if type_error else RuntimeError)(message)

          def vector(s):
              data = s.encode('latin-1')
              try:
                  import numpy
                  return numpy.frombuffer(data, dtype=numpy.float32).copy()
              except ImportError:
                  return _mle_array.array('f', data)

          def floats(v):
              # A 1-dimensional sequence of numbers as float32 bytes (latin-1 text for Java).
              try:
                  import numpy
                  return numpy.asarray(v, dtype=numpy.float32).tobytes().decode('latin-1')
              except ImportError:
                  return _mle_array.array('f', v).tobytes().decode('latin-1')

          # A parameter as Java sees it: (kind, value). 0 None, 1 bool, 2 str, 3 int (64 bits),
          # 4 float, 5 big int (text), 7 VECTOR (float32 bytes as latin-1 text), 8 bytes (latin-1
          # text), 9 JSON text, 10 Decimal (text), 11 datetime, 12 date, 13 timedelta (us), 14 time.
          def kind(v):
              if v is None: return (0, None)
              if isinstance(v, bool): return (1, v)
              if isinstance(v, int): return (3, v) if -2**63 <= v < 2**63 else (5, str(v))
              if isinstance(v, float):
                  if not _mle_math.isfinite(v): raise TypeError('cannot bind ' + repr(v))
                  return (4, v)
              if isinstance(v, str): return (2, v)
              if isinstance(v, _mle_decimal.Decimal):
                  if not v.is_finite(): raise TypeError('cannot bind ' + str(v))
                  return (10, str(v))
              if isinstance(v, _mle_dt.datetime):
                  # An aware datetime keeps its offset: Java converts it to the session's zone.
                  return (11, v.isoformat())
              if isinstance(v, _mle_dt.date): return (12, v.isoformat())
              if isinstance(v, _mle_dt.timedelta): return (13, (v.days * 86400 + v.seconds) * 1000000 + v.microseconds)
              if isinstance(v, _mle_dt.time): return (14, v.replace(tzinfo=None).isoformat())
              if isinstance(v, (bytes, bytearray, memoryview)): return (8, bytes(v).decode('latin-1'))
              if isinstance(v, _mle_array.array): return (7, floats(v))
              if type(v).__module__ == 'numpy':
                  if getattr(v, 'ndim', None) == 0: return kind(v.item())
                  if getattr(v, 'ndim', None) == 1: return (7, floats(v))
              if isinstance(v, (set, frozenset)): return (2, ','.join(map(str, v)))
              if isinstance(v, (list, tuple, dict)): return (9, _mle_json.dumps(v))
              raise TypeError('cannot bind a value of type ' + type(v).__name__)

          def timedelta(us):
              return _mle_dt.timedelta(microseconds=us)

          return {
              'list': lambda a: list(a), 'bigint': lambda s: int(s), 'decimal': _mle_decimal.Decimal,
              'bytes': lambda s: s.encode('latin-1'), 'vector': vector, 'json': _mle_json.loads,
              'date': _mle_dt.date.fromisoformat, 'datetime': _mle_dt.datetime.fromisoformat,
              'timedelta': timedelta, 'kind': kind, 'throw_sql': throw_sql, 'throw_error': throw_error,
              'load_library': load_library, 'bind': bind,
          }

      _mle_sql_api
      """;

  // ------------------------------------------------------------ Lang

  @Override
  public Object list(List<?> elements) {
    return list.execute((Object) elements.toArray());
  }

  @Override
  public RuntimeException sqlError(MleSqlException e) {
    try {
      throwSql.execute(e.getErrorCode(), e.getSqlState(), e.getMessage());
    } catch (PolyglotException p) {
      return p;
    }
    return e;
  }

  @Override
  public RuntimeException error(RuntimeException e) {
    boolean type = e instanceof IllegalArgumentException || e instanceof Values.ConversionException;
    try {
      throwError.execute(String.valueOf(e.getMessage()), type);
    } catch (PolyglotException p) {
      return p;
    }
    return e;
  }

  /** snake_case (run_sql) as the API's camelCase (runSql); names starting with _ unchanged. */
  @Override
  public String member(String name) {
    if (name.indexOf('_') < 0 || name.startsWith("_")) return name;
    StringBuilder b = new StringBuilder(name.length());
    boolean up = false;
    for (int i = 0; i < name.length(); i++) {
      char ch = name.charAt(i);
      if (ch == '_') {
        up = true;
      } else {
        b.append(up ? Character.toUpperCase(ch) : ch);
        up = false;
      }
    }
    return b.toString();
  }

  /** A dict of options; keys in camelCase or snake_case (pass_result_to_client). */
  @Override
  public SqlOptions options(Value v) {
    if (v == null || v.isNull()) return null;
    if (!v.hasHashEntries()) throw new IllegalArgumentException("options must be a dict");
    SqlOptions o = new SqlOptions();
    Value keys = v.getHashKeysIterator();
    while (keys.hasIteratorNextElement()) {
      Value k = keys.getIteratorNextElement();
      Value x = v.getHashValue(k);
      if (x.isNull()) continue;
      o.set(member(k.asString()), x.isBoolean() ? (Object) x.asBoolean() : x.isString() ? x.asString() : x.toString());
    }
    return o;
  }

  @Override
  public SqlOptions defaults() {
    return new SqlOptions().decimalType(SqlOptions.DECIMAL_TYPES.get(options.decimalType));
  }

  // ------------------------------------------------------------ columns (numpy)

  private Value helper(String name) {
    return context.getBindings("python").getMember("_mle_helpers").getHashValue(name);
  }

  /**
   * fetch_columns (Phase 7, S-14): VECTOR columns as one n x d float32 numpy matrix, integer
   * and floating-point columns as numpy int64/float64 arrays (one copy each), other columns (or
   * columns with NULLs) as lists of Python values; a dict by column label.
   */
  @Override
  public Object columns(com.percona.mle.sql.SqlResult r, List<String> names) {
    List<Object[]> rows = r.fetchRemainingRaw();
    int n = rows.size();
    Value dict = helper("dict").execute();
    for (String name : names) {
      int i = GuestSql.columnIndex(r, name);
      dict.putHashEntry(name, column(r, rows, i, n));
    }
    return dict;
  }

  @SuppressWarnings("restricted")  // the arrays' own memory, as numpy reports it
  private Object column(com.percona.mle.sql.SqlResult r, List<Object[]> rows, int i, int n) {
    boolean vectors = n > 0, longs = n > 0, doubles = n > 0;
    int dims = -1;
    for (Object[] row : rows) {
      Object v = row[i];
      vectors &= v instanceof float[] f && (dims < 0 || f.length == dims);
      if (v instanceof float[] f && dims < 0) dims = f.length;
      longs &= v instanceof Long;
      doubles &= v instanceof Double || v instanceof Float || v instanceof Long;
    }
    try {
      if (vectors) {
        Value t = helper("numpy_matrix").execute(n, dims);
        java.lang.foreign.MemorySegment m = java.lang.foreign.MemorySegment.ofAddress(t.getArrayElement(1).asLong())
            .reinterpret((long) n * dims * 4);
        long at = 0;
        for (Object[] row : rows) {
          java.lang.foreign.MemorySegment.copy(java.lang.foreign.MemorySegment.ofArray((float[]) row[i]), 0, m, at,
              dims * 4L);
          at += dims * 4L;
        }
        return t.getArrayElement(0);
      }
      if (longs || doubles) {
        Value t = helper("numpy_numbers").execute(n, longs ? "i" : "f");
        java.lang.foreign.MemorySegment m = java.lang.foreign.MemorySegment.ofAddress(t.getArrayElement(1).asLong())
            .reinterpret(n * 8L);
        for (int k = 0; k < n; k++) {
          Object v = rows.get(k)[i];
          if (longs) m.setAtIndex(java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED, k, (Long) v);
          else m.setAtIndex(java.lang.foreign.ValueLayout.JAVA_DOUBLE_UNALIGNED, k, ((Number) v).doubleValue());
        }
        return t.getArrayElement(0);
      }
    } catch (PolyglotException e) {
      if (!String.valueOf(e.getMessage()).contains("No module named")) throw e;  // no numpy: lists
    }
    List<Object> values = new java.util.ArrayList<>(n);
    com.percona.mle.sql.Column c = r.getColumns().get(i);
    for (Object[] row : rows) values.add(row[i] == null ? null : value(row[i], c, r.getOptions()));
    return list(values);
  }

  // ------------------------------------------------------------ values

  @Override
  public Object value(Object raw, Column c, SqlOptions o) {
    return switch (raw) {
      case null -> null;
      case Long l -> l;
      case BigInteger b -> bigint.execute(b.toString());
      case Float f -> (double) f;
      case Double d -> d;
      case BigDecimal d -> "NUMBER".equals(o.decimalType()) ? (Object) Double.parseDouble(d.toString())
          : decimal.execute(d.toPlainString());
      case String s -> s;
      case byte[] b -> bytes.execute(new String(b, StandardCharsets.ISO_8859_1));
      case MleJson j -> json.execute(j.text());
      case float[] f -> {
        ByteBuffer bb = ByteBuffer.allocate(f.length * 4).order(ByteOrder.LITTLE_ENDIAN);
        bb.asFloatBuffer().put(f);
        yield vector.execute(new String(bb.array(), StandardCharsets.ISO_8859_1));
      }
      case Values.Temporal t -> temporal(t);
      default -> raw;
    };
  }

  private Object temporal(Values.Temporal t) {
    if (t.tag() == Wire.T_TIME) {
      long us = ((t.hour() * 60 + t.minute()) * 60 + t.second()) * 1_000_000L + t.micro();
      return timedelta.execute(t.negative() ? -us : us);
    }
    Object j;
    try {
      j = t.toJava();
    } catch (Values.ConversionException e) {
      return null;  // a zero date
    }
    return j instanceof LocalDate d ? date.execute(d.toString()) : datetime.execute(j.toString());
  }

  @Override
  public Object param(Object v) {
    if (!(v instanceof Value x)) return v;  // a Java value
    if (x.isNull()) return null;
    if (x.isHostObject()) return x.asHostObject();
    if (x.isProxyObject() && x.asProxyObject() instanceof GuestSql.GArgument)
      throw new IllegalArgumentException("mysql.arg() values are for OUT and INOUT parameters of procedures");
    Value k = kind.execute(x);
    Value p = k.getArrayElement(1);
    return switch (k.getArrayElement(0).asInt()) {
      case 0 -> null;
      case 1 -> p.asBoolean() ? 1L : 0L;
      case 2 -> p.asString();
      case 3 -> p.asLong();
      case 4 -> p.asDouble();
      case 5 -> new BigInteger(p.asString());
      case 7 -> {
        byte[] b = p.asString().getBytes(StandardCharsets.ISO_8859_1);
        float[] f = new float[b.length / 4];
        ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(f);
        yield f;
      }
      case 8 -> p.asString().getBytes(StandardCharsets.ISO_8859_1);
      case 9 -> new MleJson(p.asString());
      case 10 -> new BigDecimal(p.asString());
      case 11 -> {
        String text = p.asString();
        java.time.temporal.TemporalAccessor parsed = java.time.format.DateTimeFormatter.ISO_DATE_TIME.parse(text);
        yield parsed.isSupported(java.time.temporal.ChronoField.OFFSET_SECONDS)
            ? java.time.OffsetDateTime.from(parsed).atZoneSameInstant(options.zone).toLocalDateTime()
            : LocalDateTime.parse(text);
      }
      case 12 -> LocalDate.parse(p.asString());
      case 13 -> Duration.ofNanos(Math.multiplyExact(p.asLong(), 1000L));
      case 14 -> LocalTime.parse(p.asString());
      default -> throw new IllegalArgumentException("cannot bind " + x);
    };
  }
}
