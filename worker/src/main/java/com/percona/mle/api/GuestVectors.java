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
 * JavaScript and Python vectors as float[] for the vector helper. Guest numbers are doubles, so
 * the host's own conversion to float[] fails for most of them (it must be lossless); these
 * round to float. numpy arrays are read in bulk (astype float32, tobytes); a Float32Array, a
 * JavaScript array, a Python list or tuple element by element; a Java float[] as it is.
 */
final class GuestVectors {
  private GuestVectors() {}

  static float[] vector(Value v, String what) {
    if (v == null || v.isNull()) throw new IllegalArgumentException(what + " must not be null");
    if (v.isHostObject()) {
      Object o = v.asHostObject();
      if (o instanceof float[] f) return f;
      if (o instanceof double[] d) {
        float[] f = new float[d.length];
        for (int i = 0; i < d.length; i++) f[i] = (float) d[i];
        return f;
      }
    }
    int[] shape = numpyShape(v);
    if (shape != null) {
      if (shape.length != 1)
        throw new IllegalArgumentException(
            what + " must be one vector, not an array of " + shape.length + " dimensions");
      return numpyFloats(v, elements(shape[0], 1, what));
    }
    float[] typed = float32Array(v);
    if (typed != null) return typed;
    if (!v.hasArrayElements()) throw new IllegalArgumentException(what + " is not a vector: " + describe(v));
    long n = v.getArraySize();
    if (n > Integer.MAX_VALUE) throw new IllegalArgumentException(what + " is too long");
    float[] f = new float[(int) n];
    for (int i = 0; i < f.length; i++) f[i] = number(v.getArrayElement(i), what, i);
    return f;
  }

  static float[][] matrix(Value v, String what) {
    if (v == null || v.isNull()) throw new IllegalArgumentException(what + " must not be null");
    if (v.isHostObject() && v.asHostObject() instanceof float[][] m) return m;
    int[] shape = numpyShape(v);
    if (shape != null) {
      if (shape.length != 2)
        throw new IllegalArgumentException(what + " must be an array of 2 dimensions, not " + shape.length);
      float[] all = numpyFloats(v, elements(shape[0], shape[1], what));
      float[][] m = new float[shape[0]][];
      for (int i = 0; i < m.length; i++) m[i] = java.util.Arrays.copyOfRange(all, i * shape[1], (i + 1) * shape[1]);
      return m;
    }
    if (!v.hasArrayElements()) throw new IllegalArgumentException(what + " is not a list of vectors: " + describe(v));
    long n = v.getArraySize();
    if (n > Integer.MAX_VALUE) throw new IllegalArgumentException(what + " has too many vectors");
    float[][] m = new float[(int) n][];
    for (int i = 0; i < m.length; i++) m[i] = vector(v.getArrayElement(i), what + " row " + i);
    return m;
  }

  /** rows x columns as an int, or a clear error (a float[] holds at most 2^31 - 1 elements). */
  private static int elements(int rows, int columns, String what) {
    long n = (long) rows * columns;
    if (rows < 0 || columns < 0 || n > Integer.MAX_VALUE - 8)
      throw new IllegalArgumentException(what + " is too large: " + rows + " x " + columns + " elements");
    return (int) n;
  }

  /**
   * A JavaScript Float32Array: one copy of its bytes (as Polyglot's VECTOR conversion), not one
   * interop call per element. Null for anything else.
   */
  private static float[] float32Array(Value v) {
    if (!v.hasArrayElements() || !v.hasMember("buffer")) return null;
    Value meta = v.getMetaObject();
    if (meta == null || !"Float32Array".equals(meta.getMetaSimpleName())) return null;
    Value buffer = v.getMember("buffer");
    long offset = v.getMember("byteOffset").asLong(), length = v.getMember("byteLength").asLong();
    if (!buffer.hasBufferElements() || length > Integer.MAX_VALUE - 8) return null;
    byte[] raw = new byte[(int) length];
    buffer.readBuffer(offset, raw, 0, raw.length);
    float[] f = new float[raw.length / 4];
    ByteBuffer.wrap(raw).order(ByteOrder.nativeOrder()).asFloatBuffer().get(f);
    return f;
  }

  private static float number(Value e, String what, int i) {
    if (e.isNumber() && e.fitsInDouble()) return (float) e.asDouble();
    if (e.canInvokeMember("item")) {  // a numpy scalar
      Value x = e.invokeMember("item");
      if (x.isNumber() && x.fitsInDouble()) return (float) x.asDouble();
    }
    throw new IllegalArgumentException(what + ": element " + i + " is not a number: " + describe(e));
  }

  /** The shape of a numpy array, or null. */
  private static int[] numpyShape(Value v) {
    if (v.isHostObject() || !v.hasMember("tobytes") || !v.hasMember("dtype") || !v.hasMember("shape")) return null;
    Value s = v.getMember("shape");
    if (!s.hasArrayElements()) return null;
    int[] shape = new int[(int) s.getArraySize()];
    for (int i = 0; i < shape.length; i++) {
      Value x = s.getArrayElement(i);
      if (!x.fitsInInt()) return null;
      shape[i] = x.asInt();
    }
    return shape;
  }

  /** A numpy array's n elements as float32, in C order. */
  private static float[] numpyFloats(Value v, int n) {
    Value a = v.getMember("dtype").getMember("str").asString().equals("<f4") ? v : v.invokeMember("astype", "<f4");
    Value bytes = a.invokeMember("tobytes");
    float[] f = new float[n];
    if (bytes.hasBufferElements() && bytes.getBufferSize() == 4L * n && n <= (Integer.MAX_VALUE - 8) / 4) {
      byte[] raw = new byte[4 * n];
      bytes.readBuffer(0, raw, 0, raw.length);
      ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(f);
      return f;
    }
    Value flat = a.invokeMember("ravel").invokeMember("tolist");  // no buffer: element by element
    for (int i = 0; i < n; i++) f[i] = number(flat.getArrayElement(i), "the array", i);
    return f;
  }

  private static String describe(Value v) {
    String s = v.toString();
    return s.length() > 60 ? s.substring(0, 60) + "..." : s;
  }
}
