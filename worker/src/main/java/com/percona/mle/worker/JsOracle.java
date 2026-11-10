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
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.RoundingMode;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.util.List;
import java.util.regex.Pattern;
import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.Source;
import org.graalvm.polyglot.Value;

/**
 * JavaScript routines in Oracle MySQL's form (CREATE ... LANGUAGE JAVASCRIPT AS $$ body $$): the
 * body becomes a function whose parameters are the routine's parameters, with Oracle's value
 * conversions (MySQL manual, "JavaScript stored program data types").
 *
 * <p>The wrapper, all on the body's first line so that line numbers match the body:
 *
 * <pre>
 * (function($c){const $G=globalThis;return function $mle$routine($a0,..){"use strict";
 *   const globalThis=Object.create($G);let p0=$c.int($a0),..;const $r=(()=>{BODY
 * })();return $r;}})
 * </pre>
 *
 * <ul>
 *   <li>globalThis is a fresh object per invocation (S-1): what a call stores there is gone
 *       afterwards; built-ins are reached as usual. Strict mode, as in Oracle.
 *   <li>Arguments are converted inside the wrapper ($c.*), so a call is one host-to-guest call.
 *   <li>The body runs in an arrow function: it can return early, and what it declares shadows the
 *       parameters (Oracle: "var x" in the body hides the parameter x). With OUT and INOUT
 *       parameters the wrapper returns [result, out1, ...]; OUT starts as null.
 *   <li>A body that uses await runs as an async function; the promise has settled when control
 *       returns to the host (pending jobs run then), and {@code settle} reads it.
 * </ul>
 */
final class JsOracle {
  private JsOracle() {}

  /** A prepared routine; prefix is the length of the wrapper text before the body on line 1. */
  record Routine(Value fn, Values.ParamType[] params, int[] outs, Values.ParamType ret, boolean async,
      int prefix, String name) {}

  private static final Pattern AWAIT = Pattern.compile("\\bawait\\b");

  /** Name of the wrapper function around the body's arrow function; its frame
      is not counted against the frame limit (the arrow is the routine's). */
  static final String WRAPPER = "$mle$routine";

  /** Converters used by the wrappers, written in JavaScript; $o holds the session options. */
  static Value helpers(Context c) {
    return c.eval(Source.create("js", """
        (() => {
          const $o = { int: 0, dec: 0 };
          const big = (a) => typeof a === 'string' || typeof a === 'number' ? BigInt(a) : a;
          return {
            $o,
            // Integers come as numbers when safe, else as strings (decided in Java).
            int: (a) => a === null ? null
                : $o.int === 3 ? big(a)
                : $o.int === 1 && typeof a === 'string' ? BigInt(a) : a,
            bit: (a) => a === null ? null : typeof a === 'string' ? BigInt(a) : a,
            dec: (a) => a === null ? null : $o.dec === 1 ? Number(a) : a,
            set: (a) => a === null ? null : new Set(a === '' ? [] : a.split(',')),
            bytes: (a) => a === null ? null : new Uint8Array(new ArrayBuffer(a)),
            vector: (a) => a === null ? null : new Float32Array(new ArrayBuffer(a)),
            json: (a) => a === null ? null : JSON.parse(a),
            date: (a) => a === null ? null : new Date(a),
            settle: (p) => {
              const s = { done: false, ok: false, v: undefined };
              Promise.resolve(p).then((v) => { s.done = true; s.ok = true; s.v = v; },
                                      (e) => { s.done = true; s.v = e; });
              return s;
            },
            rethrow: (e) => { throw e; },
            str: (v) => String(v),
            json_text: (v) => {
              if (typeof v === 'string') return JSON.stringify(JSON.parse(v));
              if (typeof v === 'boolean' || typeof v === 'number' || typeof v === 'bigint') return undefined;
              return JSON.stringify(v);
            },
            set_text: (v) => v instanceof Set ? [...v].map(String).join(',') : undefined,
            date_ms: (v) => v instanceof Date ? v.getTime() : undefined,
            // Host access to the bytes needs an ArrayBuffer (typed arrays have no buffer
            // elements in the polyglot API).
            buffer: (v) => v instanceof ArrayBuffer ? v
                : ArrayBuffer.isView(v) ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : undefined,
            floats: (v) => {
              const f = v instanceof Float32Array ? v
                  : Array.isArray(v) || ArrayBuffer.isView(v) ? Float32Array.from(v) : undefined;
              return f === undefined ? undefined : f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength);
            },
          };
        })()
        """));
  }

  /** The JavaScript converter of an argument of type t, or null if none is needed. */
  private static String converter(Values.ParamType t) {
    if (t.is("SET")) return "set";
    if (t.is("BIT")) return "bit";
    if (t.is("YEAR")) return null;
    return switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> "int";
      case Wire.T_DECIMAL -> "dec";
      case Wire.T_BYTES -> "bytes";
      case Wire.T_VECTOR -> "vector";
      case Wire.T_JSON -> "json";
      case Wire.T_DATE, Wire.T_DATETIME, Wire.T_TIMESTAMP -> "date";
      default -> null;
    };
  }

  /** Compiles a body; throws PolyglotException for syntax errors. */
  static Routine prepare(Context c, Value helpers, String name, String body, List<JavaRoutines.Param> params,
      Values.ParamType ret, java.net.URI uri, List<LibraryFileSystem.Import> imports) {
    int n = params.size();
    StringBuilder pre = new StringBuilder("(function($c){const $G=globalThis;return ");
    // USING (Phase 6): each library is imported (once per worker, see LibraryFileSystem) and
    // bound to its name or alias, so the routine is async.
    boolean async = AWAIT.matcher(body).find() || !imports.isEmpty();
    if (async) pre.append("async ");
    pre.append("function " + WRAPPER + "(");
    for (int i = 0; i < n; i++) pre.append(i == 0 ? "" : ",").append("$a").append(i);
    pre.append("){\"use strict\";const globalThis=Object.create($G);");
    Values.ParamType[] types = new Values.ParamType[n];
    int outCount = 0;
    for (int i = 0; i < n; i++) {
      JavaRoutines.Param p = params.get(i);
      types[i] = p.type();
      String conv = converter(p.type());
      String init = p.mode() == Wire.MODE_OUT ? "null"
          : conv == null ? "$a" + i : "$c." + conv + "($a" + i + ")";
      pre.append(i == 0 ? "let " : ",").append(p.name()).append('=').append(init);
      if (p.mode() != Wire.MODE_IN) outCount++;
    }
    if (n > 0) pre.append(';');
    int[] outs = new int[outCount];
    // The body runs in an arrow function: it may return early, and its own
    // declarations (Oracle's myfunc: "var x") shadow the parameters.
    for (LibraryFileSystem.Import i : imports)
      pre.append("const ").append(i.binding()).append("=await import(").append(jsString(i.path())).append(");");
    pre.append("const $r=").append(async ? "await(async()=>{" : "(()=>{");
    StringBuilder post = new StringBuilder("\n");
    if (outCount == 0) {
      post.append("})();return $r;}})");
    } else {
      post.append("})();return[$r");
      for (int i = 0, k = 0; i < n; i++)
        if (params.get(i).mode() != Wire.MODE_IN) {
          outs[k++] = i;
          post.append(',').append(params.get(i).name());
        }
      post.append("];}})");
    }
    String text = pre + body + post;
    Polyglot.prefixes.put(uri, pre.length());  // also for the positions of syntax errors
    Value factory = c.eval(Source.newBuilder("js", text, name).uri(uri).buildLiteral());
    return new Routine(factory.execute(helpers), types, outs, ret, async, pre.length(), name);
  }

  private static String jsString(String s) {
    return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
  }

  // ------------------------------------------------------------ arguments

  private static final long MAX_SAFE = (1L << 53) - 1;

  /** A neutral argument in the form the wrapper's converter takes. */
  static Object hostForm(Object v, Values.ParamType t, Options o) {
    return switch (v) {
      case null -> null;
      case Long l -> {
        if (t.is("YEAR")) yield l;
        if (t.is("BIT")) yield Math.abs(l) <= MAX_SAFE ? (Object) l : Long.toUnsignedString(l);
        if (o.integerType == Wire.INT_STRING) yield l.toString();
        yield Math.abs(l) <= MAX_SAFE ? (Object) l : l.toString();
      }
      case BigInteger b -> b.toString();
      case BigDecimal d -> d.toPlainString();
      case MleJson j -> j.text();
      case byte[] b -> ByteBuffer.wrap(b);
      case Values.VectorRef ref -> {
        ByteBuffer bb = ByteBuffer.allocate(ref.dims() * 4).order(ByteOrder.LITTLE_ENDIAN);
        java.lang.foreign.MemorySegment.ofBuffer(bb).copyFrom(ref.data());
        yield bb;
      }
      case Values.Temporal tm -> switch (tm.tag()) {
        case Wire.T_TIME -> formatTime(tm);
        case Wire.T_DATE -> (double) ((LocalDate) tm.toJava()).atStartOfDay(o.zone).toInstant().toEpochMilli();
        default -> (double) ((LocalDateTime) tm.toJava()).atZone(o.zone).toInstant().toEpochMilli();
      };
      default -> v;  // Double, Float, String
    };
  }

  private static String formatTime(Values.Temporal t) {
    return String.format("%s%02d:%02d:%02d%s", t.negative() ? "-" : "", t.hour(), t.minute(), t.second(),
        t.micro() == 0 ? "" : String.format(".%06d", t.micro()));
  }

  // ------------------------------------------------------------ results

  static Values.ConversionException cannot(Value v, Values.ParamType t, Value helpers) {
    String shown;
    try {
      shown = v.isString() ? v.asString() : helpers.getMember("str").execute(v).asString();
    } catch (RuntimeException e) {
      shown = String.valueOf(v);
    }
    return new Values.ConversionException(
        "Cannot convert value '" + shown + "' to " + t.sqlName() + " from MLE");
  }

  /** Integer range of a SQL integer type: {min, max}. */
  private static BigInteger[] range(Values.ParamType t) {
    int bits = switch (t.typeName()) {
      case "TINYINT" -> 8;
      case "SMALLINT" -> 16;
      case "MEDIUMINT" -> 24;
      case "INT" -> 32;
      default -> 64;
    };
    if (t.unsigned() || t.tag() == Wire.T_U64)
      return new BigInteger[] {BigInteger.ZERO, BigInteger.ONE.shiftLeft(bits).subtract(BigInteger.ONE)};
    return new BigInteger[] {BigInteger.ONE.shiftLeft(bits - 1).negate(),
        BigInteger.ONE.shiftLeft(bits - 1).subtract(BigInteger.ONE)};
  }

  private static boolean isBigInt(Value v) {
    Value meta = v.getMetaObject();
    return meta != null && "bigint".equals(meta.getMetaSimpleName());
  }

  /** A JavaScript result (or OUT value) as a Java value of the SQL type t (Oracle's rules). */
  static Object toJava(Value v, Values.ParamType t, Options o, Value h) {
    if (v == null || v.isNull()) return null;  // null and undefined
    // Fast paths first: the common results need no meta object (a slow call).
    switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        if (v.fitsInLong() && !t.is("SET")) {
          long l = v.asLong();  // a Number with an integral value, or a BigInt
          if (t.typeName().isEmpty() || t.is("BIGINT")) {
            if (!t.unsigned() && t.tag() == Wire.T_I64) return l;
            if (l >= 0) return l;
          }
          BigInteger[] r = range(t);
          BigInteger b = BigInteger.valueOf(l);
          if (b.compareTo(r[0]) < 0 || b.compareTo(r[1]) > 0) throw cannot(v, t, h);
          return l;
        }
      }
      case Wire.T_STRING -> {
        if (v.isString() && !t.is("SET")) return v.asString();
      }
      case Wire.T_F64, Wire.T_F32 -> {
        if (v.fitsInDouble() && !v.fitsInLong()) {  // not integral, so not a BigInt
          double d = v.asDouble();
          if (!Double.isFinite(d)) throw cannot(v, t, h);
          return d;
        }
      }
      default -> {}
    }
    if (t.is("SET") || t.is("ENUM")) {
      Value s = h.getMember("set_text").execute(v);
      return s.isString() ? s.asString() : h.getMember("str").execute(v).asString();
    }
    switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        BigInteger b;
        if (v.isBoolean()) {
          b = v.asBoolean() ? BigInteger.ONE : BigInteger.ZERO;
        } else if (v.isNumber() && v.fitsInBigInteger()) {
          b = v.asBigInteger();  // integral Numbers and BigInts
        } else if (v.isNumber() && v.fitsInDouble()) {
          double d = v.asDouble();
          if (!Double.isFinite(d)) throw cannot(v, t, h);
          b = new BigDecimal(Math.floor(d + 0.5)).toBigInteger();  // Math.round
        } else if (v.isString()) {
          try {
            b = new BigDecimal(v.asString().trim()).setScale(0, RoundingMode.HALF_UP).toBigInteger();
          } catch (NumberFormatException e) {
            throw cannot(v, t, h);
          }
        } else {
          throw cannot(v, t, h);
        }
        BigInteger[] r = range(t);
        if (b.compareTo(r[0]) < 0 || b.compareTo(r[1]) > 0) throw cannot(v, t, h);
        return b.bitLength() < 64 ? (Object) b.longValue() : b;
      }
      case Wire.T_F64, Wire.T_F32 -> {
        double d;
        if (v.isBoolean()) d = v.asBoolean() ? 1 : 0;
        else if (v.isNumber() && !isBigInt(v) && v.fitsInDouble()) d = v.asDouble();
        else if (v.isString()) {
          try {
            d = Double.parseDouble(v.asString().trim());
          } catch (NumberFormatException e) {
            throw cannot(v, t, h);
          }
        } else throw cannot(v, t, h);
        if (!Double.isFinite(d)) throw cannot(v, t, h);
        return d;
      }
      case Wire.T_DECIMAL -> {
        try {
          if (v.isBoolean()) return v.asBoolean() ? BigDecimal.ONE : BigDecimal.ZERO;
          if (v.isNumber() && isBigInt(v)) return new BigDecimal(v.asBigInteger());
          if (v.isNumber() || v.isString()) {
            if (v.isNumber() && v.fitsInDouble() && !Double.isFinite(v.asDouble())) throw cannot(v, t, h);
            String s = v.isString() ? v.asString().trim() : h.getMember("str").execute(v).asString();
            return new BigDecimal(s);
          }
        } catch (NumberFormatException e) {
          throw cannot(v, t, h);
        }
        throw cannot(v, t, h);
      }
      case Wire.T_STRING -> {
        return v.isString() ? v.asString() : h.getMember("str").execute(v).asString();
      }
      case Wire.T_JSON -> {
        Value s;
        try {
          s = h.getMember("json_text").execute(v);
        } catch (org.graalvm.polyglot.PolyglotException e) {
          if (e.isGuestException()) throw cannot(v, t, h);
          throw e;
        }
        if (s.isNull()) throw cannot(v, t, h);
        return new MleJson(s.asString());
      }
      case Wire.T_BYTES -> {
        Value u = h.getMember("buffer").execute(v);
        if (u.isNull()) throw cannot(v, t, h);
        byte[] b = new byte[Math.toIntExact(u.getBufferSize())];
        u.readBuffer(0, b, 0, b.length);
        return b;
      }
      case Wire.T_VECTOR -> {
        Value f = h.getMember("floats").execute(v);
        if (f.isNull()) throw cannot(v, t, h);
        long bytes = f.getBufferSize();
        byte[] raw = new byte[Math.toIntExact(bytes)];
        f.readBuffer(0, raw, 0, raw.length);
        float[] out = new float[raw.length / 4];
        ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(out);
        return out;
      }
      case Wire.T_DATE, Wire.T_DATETIME, Wire.T_TIMESTAMP -> {
        Value ms = h.getMember("date_ms").execute(v);
        if (ms.isNull() || !Double.isFinite(ms.asDouble())) throw cannot(v, t, h);
        LocalDateTime dt = LocalDateTime.ofInstant(Instant.ofEpochMilli((long) ms.asDouble()), o.zone);
        return t.tag() == Wire.T_DATE ? dt.toLocalDate() : dt;
      }
      case Wire.T_TIME -> {
        return Polyglot.parseTime(v.isString() ? v.asString() : h.getMember("str").execute(v).asString(), t);
      }
      default -> throw cannot(v, t, h);
    }
  }
}
