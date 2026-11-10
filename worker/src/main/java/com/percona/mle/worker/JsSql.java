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
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.util.List;
import java.util.Map;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.PolyglotException;
import org.graalvm.polyglot.Source;
import org.graalvm.polyglot.Value;
import org.graalvm.polyglot.proxy.ProxyExecutable;
import org.graalvm.polyglot.proxy.ProxyObject;

/**
 * The JavaScript SQL API (Oracle's): the globals session, mysql and SqlError over the Java core
 * (GuestSql, com.percona.mle.sql), with Oracle's value conversions: integers by integerType
 * (numbers when safe, else strings, by default), DECIMAL by decimalType (strings by default),
 * dates as Date (in the session's zone), TIME as string, JSON parsed, binary strings as
 * Uint8Array, VECTOR as Float32Array.
 */
final class JsSql implements GuestSql.Lang {
  private static final long MAX_SAFE = (1L << 53) - 1;

  private final Options options;
  private final Value h;
  private final Value array;
  private final Value bigint;
  private final Value bytes;
  private final Value vector;
  private final Value json;
  private final Value date;
  private final Value kind;

  JsSql(Context c, Options options) {
    this.options = options;
    GuestSql.GSession session = new GuestSql.GSession(this);
    Map<String, Object> bridge = Map.of(
        "session", session,
        "rand", (ProxyExecutable) a -> Functions.rand(),
        "sleep", (ProxyExecutable) a -> Functions.sleep(a[0].asDouble()),
        "uuid", (ProxyExecutable) a -> Functions.uuid(),
        "isUUID", (ProxyExecutable) a -> Functions.isUUID(a[0].isNull() ? null : a[0].asString()),
        "arg", (ProxyExecutable) a -> new GuestSql.GArgument(a.length == 0 ? null : a[0]));
    h = c.eval(Source.create("js", PRELUDE)).execute(ProxyObject.fromMap(bridge));
    array = h.getMember("array");
    bigint = h.getMember("bigint");
    bytes = h.getMember("bytes");
    vector = h.getMember("vector");
    json = h.getMember("json");
    date = h.getMember("date");
    kind = h.getMember("kind");
  }

  private static final String PRELUDE = """
      (function (B) {
        'use strict';
        // Oracle: new SqlError(sql_state, message, error_number); thrown, MySQL raises it like
        // SIGNAL. Errors of SQL statements are SqlErrors too (code, sqlState, message).
        class SqlError extends Error {
          constructor(sql_state, message, error_number) {
            super(message === undefined || message === null ? '' : String(message));
            for (const [n, v] of [['sql_state', sql_state], ['error_number', error_number]])
              if (typeof v === 'number' && !Number.isFinite(v))
                throw new TypeError('SqlError: ' + n + ' must be a finite number');
            let state = sql_state === undefined || sql_state === null ? '45000' : String(sql_state);
            if (typeof sql_state === 'number') state = state.padStart(5, '0');
            Object.defineProperty(this, '$mle$sql', { value: true });
            this.sqlState = state;
            this.code = error_number === undefined || error_number === null ? 1644 : Number(error_number);
          }
          toString() {
            return 'SqlError: Error code: ' + this.code + ' Error state: ' + this.sqlState
                + ' Error message: ' + this.message;
          }
        }
        Object.defineProperty(SqlError.prototype, 'name', { value: 'SqlError', writable: true, configurable: true });
        const freeze = Object.freeze;
        const types = {};
        for (const t of ['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL',
                         'JSON', 'STRING', 'BYTES', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM',
                         'GEOMETRY', 'VECTOR'])
          types[t] = t;
        const mysql = freeze({
          rand: () => B.rand(),
          sleep: (s) => B.sleep(Number(s)),
          uuid: () => B.uuid(),
          isUUID: (v) => B.isUUID(v === undefined || v === null ? null : String(v)),
          arg: (...v) => B.arg(...v),
          IntegerType: freeze({ BIGINT: 'BIGINT', STRING: 'STRING', UNSAFE_BIGINT: 'UNSAFE_BIGINT',
                                UNSAFE_STRING: 'UNSAFE_STRING' }),
          DecimalType: freeze({ STRING: 'STRING', NUMBER: 'NUMBER' }),
          Type: freeze(types),
          SQLError: SqlError,
          SqlError,
        });
        for (const [k, v] of [['session', B.session], ['mysql', mysql], ['SqlError', SqlError]])
          Object.defineProperty(globalThis, k, { value: v, writable: false, configurable: false });
        return {
          throwSql: (code, state, message) => { throw new SqlError(state, message, code); },
          throwError: (message, type) => { throw type ? new TypeError(message) : new Error(message); },
          array: (a) => { const n = a.length, r = new Array(n); for (let i = 0; i < n; i++) r[i] = a[i]; return r; },
          bigint: (s) => BigInt(s),
          bytes: (b) => new Uint8Array(new ArrayBuffer(b)),
          vector: (b) => new Float32Array(new ArrayBuffer(b)),
          json: (s) => JSON.parse(s),
          date: (ms) => new Date(ms),
          // A parameter as Java sees it: [kind, value]. 0 null, 1 boolean, 2 string, 3 integral number,
          // 4 other number, 5 BigInt, 6 Date (ms), 7 VECTOR bytes (Float32Array), 8 bytes, 9 JSON text.
          kind: (v) => {
            switch (typeof v) {
              case 'undefined': return [0, null];
              case 'boolean': return [1, v];
              case 'string': return [2, v];
              case 'number':
                if (!Number.isFinite(v)) throw new TypeError('cannot bind ' + v);
                return Number.isSafeInteger(v) ? [3, v] : [4, v];
              case 'bigint': return [5, v.toString()];
              case 'symbol': case 'function': throw new TypeError('cannot bind a ' + typeof v);
            }
            if (v === null) return [0, null];
            if (v instanceof Date) {
              const ms = v.getTime();
              if (!Number.isFinite(ms)) throw new TypeError('cannot bind an invalid Date');
              return [6, ms];
            }
            if (v instanceof Float32Array) return [7, v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)];
            if (v instanceof ArrayBuffer) return [8, v];
            if (ArrayBuffer.isView(v)) return [8, v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)];
            if (v instanceof Set) return [2, [...v].map(String).join(',')];
            return [9, JSON.stringify(v)];
          },
        };
      })
      """;

  // ------------------------------------------------------------ Lang

  @Override
  public Object list(List<?> elements) {
    return array.execute((Object) elements.toArray());
  }

  @Override
  public RuntimeException sqlError(MleSqlException e) {
    try {
      h.getMember("throwSql").execute(e.getErrorCode(), e.getSqlState(), e.getMessage());
    } catch (PolyglotException p) {
      return p;
    }
    return e;
  }

  @Override
  public RuntimeException error(RuntimeException e) {
    boolean type = e instanceof IllegalArgumentException || e instanceof Values.ConversionException;
    try {
      h.getMember("throwError").execute(String.valueOf(e.getMessage()), type);
    } catch (PolyglotException p) {
      return p;
    }
    return e;
  }

  @Override
  public String member(String name) {
    return name;
  }

  @Override
  public SqlOptions options(Value v) {
    if (v == null || v.isNull()) return null;
    if (!v.hasMembers()) throw new IllegalArgumentException("options must be an object");
    SqlOptions o = new SqlOptions();
    for (String k : v.getMemberKeys()) {
      Value x = v.getMember(k);
      if (x.isNull()) continue;
      o.set(k, x.isBoolean() ? (Object) x.asBoolean() : x.isString() ? x.asString() : x.toString());
    }
    return o;
  }

  @Override
  public SqlOptions defaults() {
    return new SqlOptions().integerType(SqlOptions.INTEGER_TYPES.get(options.integerType))
        .decimalType(SqlOptions.DECIMAL_TYPES.get(options.decimalType));
  }

  // ------------------------------------------------------------ values

  @Override
  public Object value(Object raw, Column c, SqlOptions o) {
    return switch (raw) {
      case null -> null;
      case Long l -> {
        String t = c.getSqlType();
        if (t.equals("YEAR")) yield l;
        if (t.equals("BIT")) yield l >= 0 && l <= MAX_SAFE ? (Object) l : bigint.execute(Long.toUnsignedString(l));
        yield integer(l, o, Math.abs(l) <= MAX_SAFE);
      }
      case BigInteger b -> c.getSqlType().equals("BIT") ? bigint.execute(b.toString()) : integer(b, o, false);
      case Float f -> (double) f;
      case Double d -> d;
      case BigDecimal d -> "NUMBER".equals(o.decimalType()) ? (Object) Double.parseDouble(d.toString())
          : d.toPlainString();
      case String s -> s;
      case byte[] b -> bytes.execute(ByteBuffer.wrap(b));
      case MleJson j -> json.execute(j.text());
      case float[] f -> {
        ByteBuffer bb = ByteBuffer.allocate(f.length * 4).order(ByteOrder.LITTLE_ENDIAN);
        bb.asFloatBuffer().put(f);
        yield vector.execute(bb);
      }
      case Values.Temporal t -> temporal(t);
      default -> raw;
    };
  }

  /** An integer by integerType: numbers when safe (UNSAFE_*), strings or BigInts. */
  private Object integer(Number n, SqlOptions o, boolean safe) {
    String mode = o.integerType() == null ? "UNSAFE_STRING" : o.integerType();
    return switch (mode) {
      case "STRING" -> n.toString();
      case "BIGINT" -> bigint.execute(n.toString());
      case "UNSAFE_BIGINT" -> safe ? n : bigint.execute(n.toString());
      default -> safe ? n : n.toString();
    };
  }

  private Object temporal(Values.Temporal t) {
    if (t.tag() == Wire.T_TIME)
      return String.format("%s%02d:%02d:%02d%s", t.negative() ? "-" : "", t.hour(), t.minute(), t.second(),
          t.micro() == 0 ? "" : String.format(".%06d", t.micro()));
    double ms;
    try {
      Object j = t.toJava();
      ms = j instanceof LocalDate d ? d.atStartOfDay(options.zone).toInstant().toEpochMilli()
          : ((LocalDateTime) j).atZone(options.zone).toInstant().toEpochMilli();
    } catch (Values.ConversionException e) {
      ms = Double.NaN;  // a zero date: an invalid Date
    }
    return date.execute(ms);
  }

  @Override
  public Object param(Object v) {
    if (!(v instanceof Value x)) return v;  // a Java value
    if (x.isNull()) return null;
    if (x.isHostObject()) return x.asHostObject();
    if (x.isString()) return x.asString();
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
      case 6 -> LocalDateTime.ofInstant(java.time.Instant.ofEpochMilli((long) p.asDouble()), options.zone);
      case 7 -> {
        byte[] b = buffer(p);
        float[] f = new float[b.length / 4];
        ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(f);
        yield f;
      }
      case 8 -> buffer(p);
      default -> new MleJson(p.asString());
    };
  }

  private static byte[] buffer(Value v) {
    byte[] b = new byte[Math.toIntExact(v.getBufferSize())];
    v.readBuffer(0, b, 0, b.length);
    return b;
  }
}
