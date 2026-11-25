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
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.ZonedDateTime;

/**
 * SQL values on the wire and their Java forms.
 *
 * <p>Decoding gives a neutral form: null, Long, Long or BigInteger (BIGINT UNSIGNED), Double,
 * Float, BigDecimal, String, byte[], {@link Temporal} (DATE, TIME, DATETIME), {@link Timestamp},
 * MleJson, or {@link VectorRef} (a VECTOR still in the message body). {@link #converter} turns it
 * into a routine's parameter type; {@link #writeAs} turns a routine's result into the declared
 * SQL type.
 *
 * <p>What the host sends is checked against §7.2 (a value outside its domain is a protocol
 * failure); what guest code gives is checked before it is written, so that the worker never sends
 * a value outside its domain: such a value is a ConversionException.
 */
final class Values {
  private Values() {}

  static final class ConversionException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    ConversionException(String message) {
      super(message);
    }
  }

  /** The session's options: their zone is where a language's local time stands for a TIMESTAMP. */
  static Options options = new Options();

  /** Type of §7.4; typeName is INT, VARCHAR, SET, YEAR, BIT ... (NULL for a column without a type). */
  record ParamType(int tag, boolean unsigned, long length, long precision, long scale, String typeName) {
    ParamType(int tag, boolean unsigned, long length, long precision, long scale) {
      this(tag, unsigned, length, precision, scale, "");
    }

    String sqlName() {
      if (!typeName.isEmpty()) return typeName;
      return tag <= Wire.MAX_TAG ? Wire.TAG_NAMES[tag] : "?";
    }

    boolean is(String name) {
      return typeName.equals(name);
    }
  }

  /** A DATE, TIME or DATETIME as on the wire (Civil, §7.2). */
  record Temporal(int tag, int year, int month, int day, long hour, int minute, int second, int micro, boolean negative) {
    /**
     * A DATE or DATETIME as a JavaScript Date (milliseconds since the epoch) in zone, by Oracle's
     * rules (MySQL manual, JavaScript stored program data types): MySQL's zero date and a date
     * with a zero month or day are an invalid Date (NaN); any other date is what JavaScript's Date
     * constructor makes of its fields in local time, so an impossible day (April 31) runs on into
     * the next month.
     */
    double jsMillis(ZoneCache zone) {
      if (month == 0 || day == 0) return Double.NaN;
      try {
        // Years 0 to 99 are those years (not 1900 to 1999); the day runs on past the month's end.
        long local = (LocalDate.of(year, month, 1).toEpochDay() + day - 1) * 86_400L + hour * 3600L + minute * 60L
            + second;
        return zone.instant(local) * 1000.0 + micro / 1000;
      } catch (java.time.DateTimeException | ArithmeticException e) {
        return Double.NaN;  // a value outside every calendar (the host does not send one)
      }
    }

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

  /**
   * A TIMESTAMP: microseconds since 1970-01-01 00:00:00 UTC, or MySQL's zero timestamp. An
   * instant; where a language has only local time, it is the time in the session's zone.
   */
  record Timestamp(long micros) {
    boolean zero() {
      return micros == Wire.ZERO_TIMESTAMP;
    }

    long millis() {
      return Math.floorDiv(micros, 1000L);
    }

    Instant instant() {
      if (zero()) throw new ConversionException("the zero TIMESTAMP has no Java equivalent");
      return Instant.ofEpochSecond(Math.floorDiv(micros, 1_000_000L), Math.floorMod(micros, 1_000_000L) * 1000L);
    }

    /** The local time in the session's zone. */
    LocalDateTime local() {
      if (zero()) throw new ConversionException("the zero TIMESTAMP has no Java equivalent");
      return options.times.localDateTime(Math.floorDiv(micros, 1_000_000L), (int) Math.floorMod(micros, 1_000_000L) * 1000);
    }
  }

  /** The wire value of an instant, or -1 outside the TIMESTAMP range. */
  static long timestampMicros(Instant i) {
    long micros;
    try {
      micros = Math.addExact(Math.multiplyExact(i.getEpochSecond(), 1_000_000L), i.getNano() / 1000);
    } catch (ArithmeticException e) {
      return -1;
    }
    return micros < 0 || micros > Wire.MAX_TIMESTAMP ? -1 : micros;
  }

  /** A VECTOR in the message body; valid only until the next message is posted. */
  record VectorRef(MemorySegment data, int dims) {
    float[] toArray() {
      // Not MemorySegment.toArray: it looks up the array class's simple name on every call
      // (reflection data; ~4.5 % of a worker decoding 16 MB of VECTORs, exp note 49).
      float[] a = new float[dims];
      MemorySegment.copy(data, ValueLayout.JAVA_FLOAT_UNALIGNED, 0, a, 0, dims);
      return a;
    }
  }

  // ------------------------------------------------------------ decoding

  /** A Type of a Param or return value (§7.4). */
  static ParamType readType(MessageReader r) {
    return readColumnType(r);
  }

  /** A Type of a Column: tag 0 for a column without a type. Its tag is one the worker knows. */
  static ParamType readColumnType(MessageReader r) {
    int tag = r.u8();
    if (tag > Wire.MAX_TAG) throw ProtocolFailure.exit("a type with tag " + tag);
    return new ParamType(tag, r.bool(), r.u32(), r.u32(), r.u32(), r.str());
  }

  private static boolean decimal(String s) {
    int n = s.length(), i = 0;
    if (n == 0 || n > Wire.MAX_DECIMAL_CHARS) return false;
    if (s.charAt(0) == '-') i++;
    int digits = i;
    while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') i++;
    if (i == digits) return false;
    if (i == n) return true;
    if (s.charAt(i++) != '.') return false;
    digits = i;
    while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') i++;
    return i > digits && i == n;
  }

  /** The host keeps the domains of §7.2; a value outside them is not looked for here (§1). */
  private static Temporal readCivil(MessageReader r, int tag) {
    return new Temporal(tag, r.u16(), r.u8(), r.u8(), r.u32(), r.u8(), r.u8(), (int) r.u32(), r.bool());
  }

  /**
   * A value of the host, decoded by its own tag (§1 item 2): it is not compared with its Column's or
   * Param's type, and the language gets a value of the tag's type. The value's domain is not
   * checked either (§1): a value outside it gives guest code what it gives, and one the worker
   * cannot represent at all ends the worker.
   */
  static Object read(MessageReader r) {
    int tag = r.u8();
    return switch (tag) {
      case Wire.T_NULL -> null;
      case Wire.T_I64 -> r.u64();
      case Wire.T_U64 -> {
        long v = r.u64();
        yield v >= 0 ? (Object) v : new BigInteger(Long.toUnsignedString(v));
      }
      // Any bit pattern: the host sends no NaN or infinity, and the worker does not end for one.
      case Wire.T_F64 -> r.f64();
      case Wire.T_F32 -> r.f32();
      case Wire.T_DECIMAL -> {
        String d = r.str();
        try {
          yield new BigDecimal(d);
        } catch (NumberFormatException e) {
          throw ProtocolFailure.exit("a DECIMAL that is no number");
        }
      }
      case Wire.T_STRING -> r.lstr();
      case Wire.T_BYTES -> r.bytes();
      case Wire.T_JSON -> new MleJson(r.lstr());
      case Wire.T_DATE, Wire.T_TIME, Wire.T_DATETIME -> readCivil(r, tag);
      case Wire.T_TIMESTAMP -> new Timestamp(r.u64());
      case Wire.T_VECTOR -> {
        long dims = r.u32();
        r.skipPadding(Wire.VECTOR_ALIGNMENT);
        // The slice is checked against the body, which is far below 2^31 elements.
        yield new VectorRef(r.slice(dims * 4L), (int) dims);
      }
      default -> throw ProtocolFailure.exit("unknown value tag " + tag);
    };
  }

  /**
   * The plain Java form of a neutral value (for SQL results and Object parameters; Appendix B.4):
   * DATE LocalDate, TIME Duration, DATETIME LocalDateTime, TIMESTAMP Instant, DECIMAL BigDecimal.
   */
  static Object toJava(Object v) {
    if (v instanceof Temporal t) return t.toJava();
    if (v instanceof Timestamp t) return t.instant();
    if (v instanceof VectorRef ref) return ref.toArray();
    return v;
  }

  // ------------------------------------------------------------ encoding

  static void writeNull(MessageWriter w) {
    w.u8(Wire.T_NULL);
  }

  private static void writeTemporal(
      MessageWriter w, int tag, int year, int month, int day, long hour, int minute, int second, int micro, boolean neg) {
    // The wire's year is 16 bits and MySQL's range ends at 9999 (a JavaScript Date reaches 275760).
    if (year < 0 || year > 9999) throw new ConversionException("the year " + year + " is out of range (0 to 9999)");
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

  /**
   * An instant (§7.2): a TIMESTAMP; outside the wire's range, a DATETIME of its local time in the
   * zone of OPTIONS, or for a declared TIMESTAMP a conversion error.
   */
  private static void writeTimestamp(MessageWriter w, Instant i, boolean declared) {
    long micros = timestampMicros(i);
    if (micros >= 0) {
      w.u8(Wire.T_TIMESTAMP);
      w.u64(micros);
      return;
    }
    if (declared) throw new ConversionException("TIMESTAMP value out of range: " + i);
    LocalDateTime local;
    try {
      local = options.times.localDateTime(i.getEpochSecond(), i.getNano());
    } catch (java.time.DateTimeException e) {
      throw new ConversionException("the instant " + i + " is out of range");
    }
    writeDateTime(w, Wire.T_DATETIME, local);
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
    // Before abs(), which overflows for the most negative Duration.
    if (Math.abs(d.getSeconds() / 3600) > Wire.MAX_TIME_HOUR) throw new ConversionException("TIME value out of range: " + d);
    Duration a = d.abs();
    long hours = a.toHours();
    if (hours > Wire.MAX_TIME_HOUR || (hours == Wire.MAX_TIME_HOUR && a.toMinutesPart() == 59
        && a.toSecondsPart() == 59 && a.toNanosPart() >= 1000))
      throw new ConversionException("TIME value out of range: " + d);
    writeTemporal(w, Wire.T_TIME, 0, 0, 0, hours, a.toMinutesPart(), a.toSecondsPart(), a.toNanosPart() / 1000, neg);
  }

  private static void writeVector(MessageWriter w, float[] f) {
    if (f.length < 1 || f.length > Wire.MAX_VECTOR_DIMENSIONS)
      throw new ConversionException("a VECTOR has 1 to " + Wire.MAX_VECTOR_DIMENSIONS + " dimensions, not " + f.length);
    w.u8(Wire.T_VECTOR);
    w.u32(f.length);
    w.align(Wire.VECTOR_ALIGNMENT);
    w.floats(f);
  }

  private static void writeVector(MessageWriter w, MemorySegment s) {
    long dims = s.byteSize() / 4;
    if (s.byteSize() % 4 != 0 || dims < 1 || dims > Wire.MAX_VECTOR_DIMENSIONS)
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
      // BIGINT if it fits, else BIGINT UNSIGNED, else DECIMAL.
      case BigInteger b -> {
        if (b.bitLength() <= 63) {
          i64(w, b.longValue());
        } else if (b.signum() >= 0 && b.bitLength() <= 64) {
          w.u8(Wire.T_U64);
          w.u64(b.longValue());
        } else {
          writeString(w, Wire.T_DECIMAL, decimalText(new BigDecimal(b), "SQL"));
        }
      }
      case Double d -> {
        // Not SQL values, in any language (JavaScript and Python refuse them before this).
        if (!Double.isFinite(d)) throw new IllegalArgumentException("cannot bind " + d);
        w.u8(Wire.T_F64);
        w.f64(d);
      }
      case Float f -> {
        if (!Float.isFinite(f)) throw new IllegalArgumentException("cannot bind " + f);
        w.u8(Wire.T_F32);
        w.f32(f);
      }
      case BigDecimal d -> writeString(w, Wire.T_DECIMAL, decimalText(d, "SQL"));
      case String s -> writeString(w, Wire.T_STRING, s);
      case MleJson j -> writeString(w, Wire.T_JSON, j.text());
      case byte[] b -> {
        w.u8(Wire.T_BYTES);
        w.bytes(b);
      }
      case LocalDate d -> writeDate(w, Wire.T_DATE, d);
      case LocalDateTime t -> writeDateTime(w, Wire.T_DATETIME, t);
      case Instant i -> writeTimestamp(w, i, false);
      case OffsetDateTime t -> writeTimestamp(w, t.toInstant(), false);
      case ZonedDateTime t -> writeTimestamp(w, t.toInstant(), false);
      case LocalTime t -> writeTime(w, Duration.ofNanos(t.toNanoOfDay()));
      case Duration d -> writeTime(w, d);
      case float[] f -> writeVector(w, f);
      case MemorySegment s -> writeVector(w, s);
      default -> throw new ConversionException("cannot pass a " + v.getClass().getName() + " to SQL");
    }
  }

  /** MySQL's DECIMAL limits: 65 digits, 30 of them after the point. */
  private static final int DECIMAL_MAX_DIGITS = 65, DECIMAL_MAX_SCALE = 30;

  /**
   * d as DECIMAL text mysqld accepts (digits and a point, at most 100 characters: §7.2, and the
   * host ends a worker that sends anything else). Digits after the 30th decimal place are rounded as
   * MySQL rounds them; more than 65 digits before the point is an error. Nothing is expanded
   * before it is known to be short ('1e2000000000' has a short BigDecimal and a huge plain text).
   */
  static String decimalText(BigDecimal d, String target) {
    int scale = d.scale();
    if (scale < 0 || scale > DECIMAL_MAX_SCALE || d.precision() > DECIMAL_MAX_DIGITS) {  // unusual
      long integerDigits = (long) d.precision() - scale;
      if (d.signum() == 0 || integerDigits < -DECIMAL_MAX_SCALE) return "0";
      if (integerDigits > DECIMAL_MAX_DIGITS)
        throw new ConversionException("value " + (d.precision() <= 40 ? d.toString() : "of " + integerDigits
            + " digits") + " is out of range for " + (target.equals("SQL") ? "a DECIMAL parameter" : target));
      if (scale > DECIMAL_MAX_SCALE) d = d.setScale(DECIMAL_MAX_SCALE, java.math.RoundingMode.HALF_UP);
      if ((long) d.precision() - d.scale() > DECIMAL_MAX_DIGITS)  // rounded up to one digit more
        throw new ConversionException("value is out of range for " + (target.equals("SQL") ? "a DECIMAL parameter" : target));
    }
    String text = d.toPlainString();
    if (!decimal(text)) throw new ConversionException("value is out of range for "
        + (target.equals("SQL") ? "a DECIMAL parameter" : target));
    return text;
  }

  private static void i64(MessageWriter w, long v) {
    w.u8(Wire.T_I64);
    w.u64(v);
  }

  /**
   * v as writeAs takes it without running routine code. A value of a class of the JDK or of this
   * worker is returned as it is: writeAs reads it in the same pass as it writes it. A value of a
   * class of the routine (a Java routine's own Number or object) is converted here, with the
   * routine's methods (toString, doubleValue), so that writeAs runs none of them while it composes
   * the RESULT (§4.1). The caller runs this while the request still counts for interrupts and its
   * SQL and imports are refused. A failure of the routine's method is a conversion error.
   */
  static Object settle(Object v, ParamType t) {
    if (v == null) return null;
    ClassLoader l = v.getClass().getClassLoader();
    // A polyglot value (a Java routine may return one it got from JavaScript or Python) shows
    // itself with guest code.
    if ((l == null || l == Values.class.getClassLoader() || l == ClassLoader.getPlatformClassLoader())
        && !(v instanceof org.graalvm.polyglot.Value))
      return v;
    try {
      return switch (t.tag()) {
        case Wire.T_STRING -> v instanceof float[] ? v : String.valueOf(v);
        case Wire.T_JSON -> v instanceof CharSequence c ? c.toString() : v;
        case Wire.T_F64, Wire.T_F32 -> v instanceof Number n ? (Object) n.doubleValue() : v;
        default -> v;  // writeAs takes no other value of a routine's class: it refuses it by its class name
      };
    } catch (RuntimeException e) {
      ConversionException c = new ConversionException("cannot convert the routine's " + v.getClass().getName()
          + " to " + t.sqlName() + ": " + e);
      c.initCause(e);
      throw c;
    }
  }

  /** A routine result as the declared SQL type t. */
  static void writeAs(MessageWriter w, Object v, ParamType t) {
    if (v == null) {
      writeNull(w);
      return;
    }
    switch (t.tag()) {
      case Wire.T_I64, Wire.T_U64 -> {
        boolean unsigned = t.unsigned() || t.tag() == Wire.T_U64;
        // The usual case without a BigInteger (exp note 37: it was 40 % of the garbage per call):
        // a Java integer always fits 64 signed bits, and fits unsigned unless negative.
        long small = 0;
        boolean isSmall = true;
        switch (v) {
          case Long l -> small = l;
          case Integer i -> small = i;
          case Short h -> small = h;
          case Byte y -> small = y;
          default -> isSmall = false;
        }
        if (isSmall) {
          if (unsigned && small < 0) throw range(v, t);
          w.u8(unsigned ? Wire.T_U64 : Wire.T_I64);
          w.u64(small);
          return;
        }
        BigInteger b = toBigInteger(v, t);
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
        double d = toNumber(v, t).doubleValue();
        if (!Double.isFinite(d)) throw cannot(v, t);
        w.u8(Wire.T_F64);
        w.f64(d);
      }
      case Wire.T_F32 -> {
        // A double beyond FLOAT's range is the server's to refuse: the host checks every float.
        double d = toNumber(v, t).doubleValue();
        if (!Double.isFinite(d)) throw cannot(v, t);
        w.u8(Wire.T_F32);
        w.f32((float) d);
      }
      case Wire.T_DECIMAL -> writeString(w, Wire.T_DECIMAL, decimalText(toBigDecimal(v, t), t.sqlName()));
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
      // An instant goes as TIMESTAMP and a value without a zone as DATETIME (§7.2), whichever of
      // the two is declared: the host converts it to the declared type as the server assigns it.
      case Wire.T_DATETIME, Wire.T_TIMESTAMP -> {
        boolean declared = t.tag() == Wire.T_TIMESTAMP;
        switch (v) {
          case LocalDateTime d -> writeDateTime(w, Wire.T_DATETIME, d);
          case LocalDate d -> writeDateTime(w, Wire.T_DATETIME, d.atStartOfDay());
          case Instant i -> writeTimestamp(w, i, declared);
          case OffsetDateTime d -> writeTimestamp(w, d.toInstant(), declared);
          case ZonedDateTime d -> writeTimestamp(w, d.toInstant(), declared);
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
        Object j = v instanceof Temporal t ? t.toJava() : v instanceof Timestamp t ? t.local() : v;
        if (!c.isInstance(j)) throw wrong(v, c);
        return j;
      });
    if (c == Instant.class)
      return nonNull(c, (v, a) -> {
        if (!(v instanceof Timestamp t)) throw wrong(v, c);
        return t.instant();
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
        // message (e.g. SQL from the routine). Closed, and so unusable, after the call.
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
      case Timestamp t -> "TIMESTAMP";
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
        case Double d when d == Math.rint(d) && d >= -0x1p63 && d < 0x1p63 -> d.longValue();
        case Float f when f == Math.rint(f) && f >= -0x1p63f && f < 0x1p63f -> f.longValue();
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
