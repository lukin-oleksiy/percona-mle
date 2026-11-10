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
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;

/**
 * SQL values on the wire and their Java forms.
 *
 * <p>Decoding gives a neutral form: null, Long, Long or BigInteger (BIGINT UNSIGNED), Double,
 * Float, BigDecimal, String, byte[], {@link Temporal}, MleJson, or {@link VectorRef} (a VECTOR
 * still in the message body). {@link #converter} turns it into a routine's parameter type;
 * {@link #writeAs} turns a routine's result into the declared SQL type.
 */
final class Values {
  private Values() {}

  static final class ConversionException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    ConversionException(String message) {
      super(message);
    }
  }

  /** ParamType from protocol.h. */
  record ParamType(int tag, boolean unsigned, long length, long precision, long scale) {
    String sqlName() {
      return tag <= Wire.MAX_TAG ? Wire.TAG_NAMES[tag] : "?";
    }
  }

  /** A DATE, TIME, DATETIME or TIMESTAMP as on the wire (MYSQL_TIME fields). */
  record Temporal(int tag, int year, int month, int day, long hour, int minute, int second, int micro, boolean negative) {
    Object toJava() {
      try {
        return switch (tag) {
          case Wire.T_DATE -> LocalDate.of(year, month, day);
          case Wire.T_TIME -> {
            Duration d = Duration.ofHours(hour).plusMinutes(minute).plusSeconds(second).plusNanos(micro * 1000L);
            yield negative ? d.negated() : d;
          }
          default -> LocalDateTime.of(year, month, day, (int) hour, minute, second, micro * 1000);
        };
      } catch (java.time.DateTimeException e) {
        throw new ConversionException(
            "the " + Wire.TAG_NAMES[tag] + " value " + this + " has no Java equivalent (zero date?)");
      }
    }

    @Override
    public String toString() {
      return String.format("%04d-%02d-%02d %s%d:%02d:%02d.%06d", year, month, day, negative ? "-" : "", hour, minute, second, micro);
    }
  }

  /** A VECTOR in the message body; valid only until the next message is posted. */
  record VectorRef(MemorySegment data, int dims) {
    float[] toArray() {
      return data.toArray(ValueLayout.JAVA_FLOAT_UNALIGNED);
    }
  }

  // ------------------------------------------------------------ decoding

  static ParamType readParamType(MessageReader r) {
    return new ParamType(r.u8(), r.flag(), r.u32(), r.u32(), r.u32());
  }

  static Object read(MessageReader r) {
    int tag = r.u8();
    return switch (tag) {
      case Wire.T_NULL -> null;
      case Wire.T_I64 -> r.u64();
      case Wire.T_U64 -> {
        long v = r.u64();
        yield v >= 0 ? (Object) v : new BigInteger(Long.toUnsignedString(v));
      }
      case Wire.T_F64 -> r.f64();
      case Wire.T_F32 -> r.f32();
      case Wire.T_DECIMAL -> new BigDecimal(r.str());
      case Wire.T_STRING -> r.str();
      case Wire.T_BYTES -> r.bytes();
      case Wire.T_JSON -> new MleJson(r.str());
      case Wire.T_DATE, Wire.T_TIME, Wire.T_DATETIME, Wire.T_TIMESTAMP ->
          new Temporal(tag, r.u16(), r.u8(), r.u8(), r.u32(), r.u8(), r.u8(), (int) r.u32(), r.flag());
      case Wire.T_VECTOR -> {
        int dims = (int) r.u32();
        r.skipAlignment(Wire.VECTOR_ALIGNMENT);
        yield new VectorRef(r.slice(dims * 4L), dims);
      }
      default -> throw new IllegalStateException("unknown value tag " + tag);
    };
  }

  /** The plain Java form of a neutral value (for SQL results and Object parameters). */
  static Object toJava(Object v) {
    if (v instanceof Temporal t) return t.toJava();
    if (v instanceof VectorRef ref) return ref.toArray();
    return v;
  }

  // ------------------------------------------------------------ encoding

  static void writeNull(MessageWriter w) {
    w.u8(Wire.T_NULL);
  }

  private static void writeTemporal(
      MessageWriter w, int tag, int year, int month, int day, long hour, int minute, int second, int micro, boolean neg) {
    w.u8(tag);
    w.u16(year);
    w.u8(month);
    w.u8(day);
    w.u32(hour);
    w.u8(minute);
    w.u8(second);
    w.u32(micro);
    w.u8(neg ? 1 : 0);
  }

  private static void writeDate(MessageWriter w, int tag, LocalDate d) {
    writeTemporal(w, tag, d.getYear(), d.getMonthValue(), d.getDayOfMonth(), 0, 0, 0, 0, false);
  }

  private static void writeDateTime(MessageWriter w, int tag, LocalDateTime t) {
    writeTemporal(w, tag, t.getYear(), t.getMonthValue(), t.getDayOfMonth(), t.getHour(), t.getMinute(), t.getSecond(),
        t.getNano() / 1000, false);
  }

  private static void writeTime(MessageWriter w, Duration d) {
    boolean neg = d.isNegative();
    Duration a = d.abs();
    long hours = a.toHours();
    if (hours > Wire.MAX_TIME_HOUR || (hours == Wire.MAX_TIME_HOUR && a.toMinutesPart() == 59
        && a.toSecondsPart() == 59 && a.toNanosPart() >= 1000))
      throw new ConversionException("TIME value out of range: " + d);
    writeTemporal(w, Wire.T_TIME, 0, 0, 0, hours, a.toMinutesPart(), a.toSecondsPart(), a.toNanosPart() / 1000, neg);
  }

  private static void writeVector(MessageWriter w, float[] f) {
    if (f.length > Wire.MAX_VECTOR_DIMENSIONS) throw new ConversionException("VECTOR too large: " + f.length);
    w.u8(Wire.T_VECTOR);
    w.u32(f.length);
    w.align(Wire.VECTOR_ALIGNMENT);
    w.floats(f);
  }

  private static void writeVector(MessageWriter w, MemorySegment s) {
    long dims = s.byteSize() / 4;
    if (s.byteSize() % 4 != 0 || dims > Wire.MAX_VECTOR_DIMENSIONS)
      throw new ConversionException("not a VECTOR: a segment of " + s.byteSize() + " bytes");
    w.u8(Wire.T_VECTOR);
    w.u32(dims);
    w.align(Wire.VECTOR_ALIGNMENT);
    w.floats(s, dims);
  }

  private static void writeString(MessageWriter w, int tag, String s) {
    w.u8(tag);
    w.str(s);
  }

  /** A statement parameter: the SQL type follows the Java type. */
  static void writeParam(MessageWriter w, Object v) {
    switch (v) {
      case null -> writeNull(w);
      case Long l -> i64(w, l);
      case Integer i -> i64(w, i);
      case Short s -> i64(w, s);
      case Byte b -> i64(w, b);
      case Boolean b -> i64(w, b ? 1 : 0);
      case BigInteger b -> {
        if (b.signum() >= 0 && b.bitLength() <= 64) {
          w.u8(Wire.T_U64);
          w.u64(b.longValue());
        } else {
          writeString(w, Wire.T_DECIMAL, b.toString());
        }
      }
      case Double d -> {
        w.u8(Wire.T_F64);
        w.f64(d);
      }
      case Float f -> {
        w.u8(Wire.T_F32);
        w.f32(f);
      }
      case BigDecimal d -> writeString(w, Wire.T_DECIMAL, d.toPlainString());
      case String s -> writeString(w, Wire.T_STRING, s);
      case MleJson j -> writeString(w, Wire.T_JSON, j.text());
      case byte[] b -> {
        w.u8(Wire.T_BYTES);
        w.bytes(b);
      }
      case LocalDate d -> writeDate(w, Wire.T_DATE, d);
      case LocalDateTime t -> writeDateTime(w, Wire.T_DATETIME, t);
      case LocalTime t -> writeTime(w, Duration.ofNanos(t.toNanoOfDay()));
      case Duration d -> writeTime(w, d);
      case float[] f -> writeVector(w, f);
      case MemorySegment s -> writeVector(w, s);
      default -> throw new ConversionException("cannot pass a " + v.getClass().getName() + " to SQL");
    }
  }

  private static void i64(MessageWriter w, long v) {
    w.u8(Wire.T_I64);
    w.u64(v);
  }

  /** A routine result as the declared SQL type t. */
  static void writeAs(MessageWriter w, Object v, ParamType t) {
    if (v == null) {
      writeNull(w);
      return;
    }
    switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        BigInteger b = toBigInteger(v, t);
        boolean unsigned = t.unsigned() || t.tag() == Wire.T_U64;
        if (unsigned) {
          if (b.signum() < 0 || b.bitLength() > 64) throw range(v, t);
          w.u8(Wire.T_U64);
        } else {
          if (b.bitLength() > 63) throw range(v, t);
          w.u8(Wire.T_I64);
        }
        w.u64(b.longValue());
      }
      case Wire.T_F64 -> {
        w.u8(Wire.T_F64);
        w.f64(toNumber(v, t).doubleValue());
      }
      case Wire.T_F32 -> {
        w.u8(Wire.T_F32);
        w.f32(toNumber(v, t).floatValue());
      }
      case Wire.T_DECIMAL -> writeString(w, Wire.T_DECIMAL, toBigDecimal(v, t).toPlainString());
      case Wire.T_STRING -> {
        if (v instanceof byte[] || v instanceof float[]) throw cannot(v, t);
        writeString(w, Wire.T_STRING, String.valueOf(v));
      }
      case Wire.T_JSON -> {
        if (!(v instanceof MleJson) && !(v instanceof CharSequence)) throw cannot(v, t);
        writeString(w, Wire.T_JSON, v.toString());
      }
      case Wire.T_BYTES -> {
        byte[] b = switch (v) {
          case byte[] a -> a;
          case String s -> s.getBytes(StandardCharsets.UTF_8);
          case MemorySegment s -> s.toArray(ValueLayout.JAVA_BYTE);
          default -> throw cannot(v, t);
        };
        w.u8(Wire.T_BYTES);
        w.bytes(b);
      }
      case Wire.T_DATE -> {
        if (!(v instanceof LocalDate d)) throw cannot(v, t);
        writeDate(w, Wire.T_DATE, d);
      }
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> {
        switch (v) {
          case LocalDateTime d -> writeDateTime(w, t.tag(), d);
          case LocalDate d -> writeDateTime(w, t.tag(), d.atStartOfDay());
          default -> throw cannot(v, t);
        }
      }
      case Wire.T_TIME -> {
        switch (v) {
          case Duration d -> writeTime(w, d);
          case LocalTime l -> writeTime(w, Duration.ofNanos(l.toNanoOfDay()));
          default -> throw cannot(v, t);
        }
      }
      case Wire.T_VECTOR -> {
        switch (v) {
          case float[] f -> writeVector(w, f);
          case MemorySegment s -> writeVector(w, s);
          case double[] d -> {
            float[] f = new float[d.length];
            for (int i = 0; i < d.length; i++) f[i] = (float) d[i];
            writeVector(w, f);
          }
          default -> throw cannot(v, t);
        }
      }
      default -> throw cannot(v, t);
    }
  }

  private static ConversionException cannot(Object v, ParamType t) {
    return new ConversionException("cannot convert " + v.getClass().getName() + " to " + t.sqlName());
  }

  private static ConversionException range(Object v, ParamType t) {
    return new ConversionException("value " + v + " is out of range for " + t.sqlName());
  }

  private static Number toNumber(Object v, ParamType t) {
    if (v instanceof Number n) return n;
    if (v instanceof Boolean b) return b ? 1 : 0;
    throw cannot(v, t);
  }

  private static BigInteger toBigInteger(Object v, ParamType t) {
    return switch (v) {
      case Long l -> BigInteger.valueOf(l);
      case Integer i -> BigInteger.valueOf(i);
      case Short s -> BigInteger.valueOf(s);
      case Byte b -> BigInteger.valueOf(b);
      case Boolean b -> b ? BigInteger.ONE : BigInteger.ZERO;
      case BigInteger b -> b;
      case BigDecimal d -> {
        try {
          yield d.toBigIntegerExact();
        } catch (ArithmeticException e) {
          throw range(v, t);
        }
      }
      case Double d when d == Math.rint(d) && !d.isInfinite() -> new BigDecimal(d).toBigIntegerExact();
      case Float f when f == Math.rint(f) && !f.isInfinite() -> new BigDecimal(f).toBigIntegerExact();
      default -> throw cannot(v, t);
    };
  }

  private static BigDecimal toBigDecimal(Object v, ParamType t) {
    return switch (v) {
      case BigDecimal d -> d;
      case BigInteger b -> new BigDecimal(b);
      case Long l -> BigDecimal.valueOf(l);
      case Integer i -> BigDecimal.valueOf(i);
      case Double d when Double.isFinite(d) -> BigDecimal.valueOf(d);
      case Float f when Float.isFinite(f) -> new BigDecimal(Float.toString(f));
      case String s -> {
        try {
          yield new BigDecimal(s);
        } catch (NumberFormatException e) {
          throw cannot(v, t);
        }
      }
      default -> throw cannot(v, t);
    };
  }

  // ------------------------------------------------------------ parameters

  /** Converts a neutral argument to a parameter type. callArena owns MemorySegment arguments. */
  interface Converter {
    Object convert(Object v, Arena callArena);
  }

  /** null if the parameter type is not supported. */
  static Converter converter(Class<?> c) {
    if (c == long.class || c == Long.class) return nonNull(c, (v, a) -> exactLong(v, c));
    if (c == int.class || c == Integer.class) return nonNull(c, (v, a) -> (int) ranged(v, c, Integer.MIN_VALUE, Integer.MAX_VALUE));
    if (c == short.class || c == Short.class) return nonNull(c, (v, a) -> (short) ranged(v, c, Short.MIN_VALUE, Short.MAX_VALUE));
    if (c == byte.class || c == Byte.class) return nonNull(c, (v, a) -> (byte) ranged(v, c, Byte.MIN_VALUE, Byte.MAX_VALUE));
    if (c == boolean.class || c == Boolean.class) return nonNull(c, (v, a) -> exactLong(v, c) != 0);
    if (c == double.class || c == Double.class) return nonNull(c, (v, a) -> number(v, c).doubleValue());
    if (c == float.class || c == Float.class) return nonNull(c, (v, a) -> number(v, c).floatValue());
    if (c == BigDecimal.class)
      return nonNull(c, (v, a) -> switch (v) {
        case BigDecimal d -> d;
        case BigInteger b -> new BigDecimal(b);
        case Long l -> BigDecimal.valueOf(l);
        case Double d -> BigDecimal.valueOf(d);
        case Float f -> new BigDecimal(Float.toString(f));
        default -> throw wrong(v, c);
      });
    if (c == BigInteger.class)
      return nonNull(c, (v, a) -> switch (v) {
        case BigInteger b -> b;
        case Long l -> BigInteger.valueOf(l);
        default -> throw wrong(v, c);
      });
    if (c == String.class)
      return nonNull(c, (v, a) -> switch (v) {
        case String s -> s;
        case MleJson j -> j.text();
        case BigDecimal d -> d.toPlainString();
        case Number n -> n.toString();
        default -> throw wrong(v, c);
      });
    if (c == byte[].class)
      return nonNull(c, (v, a) -> switch (v) {
        case byte[] b -> b;
        case String s -> s.getBytes(StandardCharsets.UTF_8);
        default -> throw wrong(v, c);
      });
    if (c == MleJson.class) return nonNull(c, (v, a) -> v instanceof MleJson j ? j : new MleJson(String.valueOf(v)));
    if (c == LocalDate.class || c == LocalDateTime.class || c == Duration.class)
      return nonNull(c, (v, a) -> {
        Object j = v instanceof Temporal t ? t.toJava() : v;
        if (!c.isInstance(j)) throw wrong(v, c);
        return j;
      });
    if (c == LocalTime.class)
      return nonNull(c, (v, a) -> {
        if (!(v instanceof Temporal t) || t.tag() != Wire.T_TIME || t.negative() || t.hour() > 23) throw wrong(v, c);
        return LocalTime.of((int) t.hour(), t.minute(), t.second(), t.micro() * 1000);
      });
    if (c == float[].class)
      return nonNull(c, (v, a) -> {
        if (!(v instanceof VectorRef ref)) throw wrong(v, c);
        return ref.toArray();
      });
    if (c == MemorySegment.class)
      return nonNull(c, (v, a) -> {
        if (!(v instanceof VectorRef ref)) throw wrong(v, c);
        // A copy in native memory owned by the call: the arena itself is overwritten by the next
        // message (e.g. Mle.sql in the routine). Closed, and so unusable, after the call.
        MemorySegment s = a.allocate(ref.data().byteSize(), Wire.VECTOR_ALIGNMENT);
        s.copyFrom(ref.data());
        return s.asReadOnly();
      });
    if (c == Object.class) return (v, a) -> toJava(v);
    return null;
  }

  private static Converter nonNull(Class<?> c, Converter conv) {
    boolean primitive = c.isPrimitive();
    return (v, a) -> {
      if (v == null) {
        if (primitive) throw new ConversionException("SQL NULL cannot be passed to a " + c.getName() + " parameter");
        return null;
      }
      return conv.convert(v, a);
    };
  }

  private static ConversionException wrong(Object v, Class<?> c) {
    String from = switch (v) {
      case Temporal t -> Wire.TAG_NAMES[t.tag()];
      case VectorRef r -> "VECTOR";
      default -> v.getClass().getSimpleName();
    };
    return new ConversionException("cannot convert " + from + " to " + c.getSimpleName());
  }

  private static Number number(Object v, Class<?> c) {
    if (v instanceof Number n) return n;
    throw wrong(v, c);
  }

  private static long exactLong(Object v, Class<?> c) {
    try {
      return switch (v) {
        case Long l -> l;
        case BigInteger b -> b.longValueExact();
        case BigDecimal d -> d.longValueExact();
        case Double d when d == Math.rint(d) && Math.abs(d) < 0x1p63 -> d.longValue();
        case Float f when f == Math.rint(f) && Math.abs(f) < 0x1p63f -> f.longValue();
        default -> throw wrong(v, c);
      };
    } catch (ArithmeticException e) {
      throw new ConversionException("value " + v + " is out of range for " + c.getSimpleName());
    }
  }

  private static long ranged(Object v, Class<?> c, long min, long max) {
    long l = exactLong(v, c);
    if (l < min || l > max) throw new ConversionException("value " + v + " is out of range for " + c.getSimpleName());
    return l;
  }
}
