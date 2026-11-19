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

package com.percona.mle.api;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.graalvm.polyglot.Value;

/**
 * JavaScript and Python columns of numbers as double[] for the statistics helper (exp note 54): a
 * numpy array in bulk (astype float64, tobytes), a Float64Array or Float32Array in one copy, a
 * JavaScript array or Python list element by element (null / None: NaN, a missing value; a Python
 * Decimal, as a DECIMAL column gives, or an integer beyond 2^63: the nearest double), a Java
 * double[], float[], int[] or long[] as they are or widened.
 */
final class GuestColumns {
  private GuestColumns() {}

  static double[] doubles(Object o, String what) {
    if (o == null) throw new IllegalArgumentException(what + " must not be null");
    if (o instanceof double[] d) return d;
    double[] w = widen(o);
    if (w != null) return w;
    Value v = GuestVectors.value(o);
    if (v.isNull()) throw new IllegalArgumentException(what + " must not be null");
    if (v.isHostObject()) {
      Object h = v.asHostObject();
      if (h instanceof double[] d) return d;
      if ((w = widen(h)) != null) return w;
    }
    if (!v.isHostObject() && v.hasMember("tobytes") && v.hasMember("dtype") && v.hasMember("shape")) {
      Value shape = v.getMember("shape");
      if (shape.getArraySize() != 1)
        throw new IllegalArgumentException(what + " must be one column, not an array of " + shape.getArraySize()
            + " dimensions");
      long n = shape.getArrayElement(0).asLong();
      if (n > (Integer.MAX_VALUE - 8) / 8) throw new IllegalArgumentException(what + " is too long");
      Value a = v.getMember("dtype").getMember("str").asString().equals("<f8") ? v : v.invokeMember("astype", "<f8");
      Value bytes = a.invokeMember("tobytes");
      double[] d = new double[(int) n];
      if (bytes.hasBufferElements() && bytes.getBufferSize() == 8 * n) {
        byte[] raw = new byte[(int) (8 * n)];
        bytes.readBuffer(0, raw, 0, raw.length);
        ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).asDoubleBuffer().get(d);
        return d;
      }
      v = a.invokeMember("tolist");
    }
    double[] typed = typedArray(v);
    if (typed != null) return typed;
    if (!v.hasArrayElements()) throw new IllegalArgumentException(what + " is not an array of numbers");
    long n = v.getArraySize();
    if (n > Integer.MAX_VALUE - 8) throw new IllegalArgumentException(what + " is too long");
    double[] d = new double[(int) n];
    for (int i = 0; i < d.length; i++) {
      Value e = v.getArrayElement(i);
      if (e.isNull()) d[i] = Double.NaN;
      else if (e.isNumber() && e.fitsInDouble()) d[i] = e.asDouble();
      else if (e.isNumber() && e.fitsInLong()) d[i] = e.asLong();
      else if (e.isBoolean()) d[i] = e.asBoolean() ? 1 : 0;
      else if (e.canInvokeMember("item")) d[i] = e.invokeMember("item").asDouble();  // a numpy scalar
      else if (e.isHostObject() && e.asHostObject() instanceof Number num) d[i] = num.doubleValue();
      else if (e.isNumber() && e.fitsInBigInteger()) d[i] = e.asBigInteger().doubleValue();  // beyond 2^63
      else if (!e.isString() && e.canInvokeMember("__float__")) d[i] = e.invokeMember("__float__").asDouble();  // Decimal
      else throw new IllegalArgumentException(what + ": element " + i + " is not a number: " + e);
    }
    return d;
  }

  private static double[] widen(Object h) {
    if (h instanceof float[] f) {
      double[] d = new double[f.length];
      for (int i = 0; i < f.length; i++) d[i] = f[i];
      return d;
    }
    if (h instanceof int[] x) {
      double[] d = new double[x.length];
      for (int i = 0; i < x.length; i++) d[i] = x[i];
      return d;
    }
    if (h instanceof long[] x) {
      double[] d = new double[x.length];
      for (int i = 0; i < x.length; i++) d[i] = x[i];
      return d;
    }
    return null;
  }

  /** A JavaScript Float64Array or Float32Array in one copy of its bytes; null for anything else. */
  private static double[] typedArray(Value v) {
    if (!v.hasArrayElements() || !v.hasMember("buffer")) return null;
    Value meta = v.getMetaObject();
    String name = meta == null ? "" : meta.getMetaSimpleName();
    if (!"Float64Array".equals(name) && !"Float32Array".equals(name)) return null;
    Value buffer = v.getMember("buffer");
    long[] range = GuestVectors.typedArrayBytes(v, buffer, name.equals("Float64Array") ? 8 : 4);
    if (range == null) return null;
    byte[] raw = new byte[(int) range[1]];
    buffer.readBuffer(range[0], raw, 0, raw.length);
    ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.nativeOrder());
    double[] d;
    if (name.equals("Float64Array")) {
      d = new double[raw.length / 8];
      b.asDoubleBuffer().get(d);
    } else {
      float[] f = new float[raw.length / 4];
      b.asFloatBuffer().get(f);
      d = new double[f.length];
      for (int i = 0; i < f.length; i++) d[i] = f[i];
    }
    return d;
  }

  /**
   * Several guest columns: an array (list) of columns, or a 2-D numpy array of rows x columns
   * (shape (observations, variables), as numpy and pandas hold a table), read by its columns.
   */
  static double[][] columns(Object o, String what) {
    if (o instanceof double[][] d) return d;
    Value v = GuestVectors.value(o);
    if (v == null || v.isNull()) throw new IllegalArgumentException(what + " must not be null");
    if (v.isHostObject() && v.asHostObject() instanceof double[][] d) return d;
    int[] shape = GuestVectors.numpyShape(v);
    if (shape != null && shape.length == 2) v = v.getMember("T");  // its elements are the columns
    if (!v.hasArrayElements()) throw new IllegalArgumentException(what + " must be an array of columns");
    long n = v.getArraySize();
    if (n > Integer.MAX_VALUE - 8) throw new IllegalArgumentException(what + " has too many columns");
    double[][] c = new double[(int) n][];
    for (int i = 0; i < c.length; i++) c[i] = doubles(v.getArrayElement(i), what + " " + i);
    return c;
  }
}
