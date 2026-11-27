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

import com.percona.mle.sql.Row;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import jdk.incubator.vector.ByteVector;
import jdk.incubator.vector.FloatVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorShape;
import jdk.incubator.vector.VectorSpecies;

/**
 * Vectors kept quantized (exp note 75): 'int8' (one byte per dimension and a scale per vector: 4
 * times smaller than floats, a scan about 2.5 times faster) or 'binary' (one bit per dimension,
 * its sign: 32 times smaller, a scan about 10 times faster). A search scans the codes for
 * k x oversample candidates and rescores them: with the float vectors when the set keeps them
 * ({@link VectorSet#quantize}: exact scores, the codes only make the scan faster), else from the
 * codes ({@link Vectors#loadQuantized}: the floats are never kept, so a set of millions of
 * vectors fits in a worker; scores are approximate).
 *
 * <p>int8: each vector scaled by its largest |value| to -127..127; scores of the float query
 * against the codes (cosine, dot, euclidean), within about 1 % of the float scores. binary:
 * candidates by Hamming distance between the signs of the query and of each vector; rescored
 * with the floats, or (without them) as the query's cosine with the vector of signs; metrics
 * cosine (and dot with the floats) and hamming (the distance in bits, no rescoring). Binary
 * codes lose recall below about 256 dimensions, and when the vectors are not centred.
 *
 * <p>Threads: the scan is split over the vector pool like {@link VectorSet#nearest}; candidates
 * are ranked by score then position, so results do not depend on the number of threads.
 */
public final class QuantizedSet {
  static final VectorSpecies<Float> SP = VectorEngine.SP;
  static final int L = VectorEngine.L;
  static final VectorSpecies<Byte> BS = VectorSpecies.of(byte.class, VectorShape.forBitSize(L * 8));
  /** At most this many candidates (k x oversample). */
  static final int CANDIDATES_MAX = 100_000;

  private final boolean binary;
  private final int d;
  private final byte[][] codes;  // int8
  private final float[] scale;  // int8: the value of code 1 per vector
  private final long[][] bits;  // binary
  private final float[] norm;  // the norm of what the codes stand for (int8: the dequantized vector)
  private final float[][] floats;  // or null
  private final long[] ids;
  private final String[] labels;
  private final int skipped;
  private Map<Long, Integer> byId;

  private QuantizedSet(boolean binary, int d, byte[][] codes, float[] scale, long[][] bits, float[] norm,
      float[][] floats, long[] ids, String[] labels, int skipped) {
    this.binary = binary;
    this.d = d;
    this.codes = codes;
    this.scale = scale;
    this.bits = bits;
    this.norm = norm;
    this.floats = floats;
    this.ids = ids;
    this.labels = labels;
    this.skipped = skipped;
  }

  static boolean binaryType(String type) {
    String t = type == null ? "" : type.trim().toLowerCase(Locale.ROOT);
    return switch (t) {
      case "int8" -> false;
      case "binary" -> true;
      default -> throw new IllegalArgumentException("unknown quantization '" + type + "' (int8 or binary)");
    };
  }

  /** Codes of one vector: {byte[] or long[], scale, norm}. */
  private static Object[] encode(float[] v, boolean binary) {
    int d = v.length;
    if (binary) {
      long[] b = new long[(d + 63) / 64];
      for (int j = 0; j < d; j++)
        if (v[j] > 0) b[j >>> 6] |= 1L << (j & 63);
      return new Object[] {b, 0f, (float) Math.sqrt(d)};
    }
    float max = 0;
    for (float x : v) max = Math.max(max, Math.abs(x));
    byte[] c = new byte[d];
    float s = max > 0 ? max / 127 : 0;
    double n2 = 0;
    for (int j = 0; j < d; j++) {
      int q = s > 0 ? Math.round(v[j] / s) : 0;
      c[j] = (byte) Math.max(-127, Math.min(127, q));
      double x = c[j] * (double) s;
      n2 += x * x;
    }
    return new Object[] {c, s, (float) Math.sqrt(n2)};
  }

  static QuantizedSet of(float[][] rows, long[] ids, String[] labels, int skipped, String type, boolean keepFloats) {
    boolean binary = binaryType(type);
    int n = rows.length, d = VectorEngine.dimension(rows);
    byte[][] codes = binary ? null : new byte[n][];
    long[][] bits = binary ? new long[n][] : null;
    float[] scale = binary ? null : new float[n], norm = new float[n];
    int parts = VectorEngine.parts((long) n * d, n);
    VectorEngine.run(parts, parts > 1, p -> {
      for (int i = (int) ((long) n * p / parts), hi = (int) ((long) n * (p + 1) / parts); i < hi; i++) {
        Object[] e = encode(rows[i], binary);
        if (binary) bits[i] = (long[]) e[0];
        else {
          codes[i] = (byte[]) e[0];
          scale[i] = (float) e[1];
        }
        norm[i] = (float) e[2];
      }
    });
    return new QuantizedSet(binary, d, codes, scale, bits, norm, keepFloats ? rows : null, ids, labels, skipped);
  }

  /** As VectorSet.load, each vector encoded as it arrives and its floats dropped. */
  static QuantizedSet load(String type, String sql, Object[] params) {
    boolean binary = binaryType(type);
    if (sql == null) throw new IllegalArgumentException("the query must not be null");
    List<byte[]> codes = new ArrayList<>();
    List<long[]> bits = new ArrayList<>();
    List<Float> scale = new ArrayList<>(), norm = new ArrayList<>();
    List<Long> idList = new ArrayList<>();
    List<String> labelList = new ArrayList<>();
    int skipped = 0, d = -1, n = 0;
    boolean withIds = false, withLabels = false;
    for (Row row : Mle.session().runSql(sql, params == null ? new Object[0] : params,
        new com.percona.mle.sql.SqlOptions().passResultToClient(false))) {
      int columns = row.getLength();
      if (n == 0 && skipped == 0) {
        if (columns < 1 || columns > 3)
          throw new IllegalArgumentException(
              "the query must return (id, vector, label), (id, vector) or (vector), not " + columns + " columns");
        withIds = columns >= 2;
        withLabels = columns == 3;
      }
      Object v = row.get(withIds ? 1 : 0);
      if (v == null) {
        skipped++;
        continue;
      }
      long id = withIds ? VectorSet.id(row.get(0), n + skipped) : n + skipped;
      if (!(v instanceof float[] f))
        throw new IllegalArgumentException("the " + (withIds ? "second" : "first") + " column must be a VECTOR, not "
            + v.getClass().getSimpleName());
      if (d < 0) d = f.length;
      else if (f.length != d)
        throw new IllegalArgumentException("the vector of row " + (n + skipped) + " (id " + id + ") has " + f.length
            + " dimensions, the first has " + d);
      Object[] e = encode(f, binary);
      if (binary) bits.add((long[]) e[0]);
      else {
        codes.add((byte[]) e[0]);
        scale.add((Float) e[1]);
      }
      norm.add((Float) e[2]);
      idList.add(id);
      if (withLabels) {
        Object l = row.get(2);
        labelList.add(l == null ? null : l instanceof byte[] b ? new String(b, java.nio.charset.StandardCharsets.UTF_8)
            : String.valueOf(l));
      }
      n++;
    }
    long[] ids = new long[n];
    float[] sc = binary ? null : new float[n], nm = new float[n];
    for (int i = 0; i < n; i++) {
      ids[i] = idList.get(i);
      nm[i] = norm.get(i);
      if (!binary) sc[i] = scale.get(i);
    }
    return new QuantizedSet(binary, Math.max(d, 0), binary ? null : codes.toArray(new byte[0][]), sc,
        binary ? bits.toArray(new long[0][]) : null, nm, null, ids, withLabels ? labelList.toArray(new String[0]) : null,
        skipped);
  }

  // ------------------------------------------------------------- the set

  public int size() { return ids.length; }

  public int dimensions() { return d; }

  /** 'int8' or 'binary'. */
  public String type() { return binary ? "binary" : "int8"; }

  /** Whether the float vectors are kept to rescore with (exact scores). */
  public boolean rescoring() { return floats != null; }

  /** The bytes of the codes (without the float vectors a rescoring set keeps too). */
  public long bytes() {
    long per = binary ? 8L * ((d + 63) / 64) : d + 4;
    return (long) size() * (per + 4);
  }

  /** Rows the load query returned with a NULL vector. */
  public int skipped() { return skipped; }

  public long id(int i) { return ids[i]; }

  public String label(int i) { return labels == null ? null : labels[i]; }

  /** The position of the vector with this id, or -1. */
  public int indexOf(long id) {
    if (byId == null) {
      Map<Long, Integer> m = new HashMap<>(ids.length * 2);
      for (int i = ids.length - 1; i >= 0; i--) m.put(ids[i], i);
      byId = m;
    }
    Integer i = byId.get(id);
    return i == null ? -1 : i;
  }

  // ---------------------------------------------------------- the search

  /** The k vectors nearest to query: metric cosine, dot, euclidean (int8), cosine, dot, hamming (binary). */
  public Matches nearest(float[] query, int k, String metric) {
    return nearest(query, k, metric, binary ? 4 : floats != null ? 2 : 1);
  }

  /**
   * {@link #nearest(float[], int, String)} with k x oversample candidates from the scan of the
   * codes to rescore (binary: 4 by default; int8: 2 with the floats, else 1).
   */
  public Matches nearest(float[] query, int k, String metric, int oversample) {
    if (query == null) throw new IllegalArgumentException("the query must not be null");
    if (query.length != d) throw new IllegalArgumentException("the query has " + query.length + " dimensions, the set " + d);
    VectorEngine.checkK(k);
    if (oversample < 1) throw new IllegalArgumentException("oversample must be at least 1, not " + oversample);
    String m = metric == null ? "" : metric.trim().toLowerCase(Locale.ROOT);
    boolean hamming = m.equals("hamming");
    int code;
    if (hamming) {
      if (!binary) throw new IllegalArgumentException("hamming is a metric of binary sets");
      code = -1;
    } else {
      code = VectorEngine.code(Vectors.Metric.of(metric));
      if (binary && code == VectorEngine.EUCLIDEAN)
        throw new IllegalArgumentException("a binary set searches by cosine, dot (with the floats) or hamming");
      if (binary && code == VectorEngine.DOT && floats == null)
        throw new IllegalArgumentException("a binary set without its floats searches by cosine or hamming");
    }
    int n = size();
    int want = (int) Math.min(n, Math.min((long) k * (hamming ? 1 : oversample), Integer.MAX_VALUE));
    if (want > CANDIDATES_MAX)
      throw new IllegalArgumentException(want + " candidates is more than " + CANDIDATES_MAX + ": a smaller k or oversample");
    float qInv = 0, q2 = 0;
    for (float x : query) q2 += x * x;
    if (q2 > 0) qInv = (float) (1 / Math.sqrt(q2));
    final long[] qb = binary ? (long[]) encode(query, true)[0] : null;
    final float fq2 = q2, fqInv = qInv;
    // The scan: keys (higher is better) of every row, the best `want` kept per part.
    int parts = VectorEngine.parts((long) n * d / (binary ? 16 : 2), n);
    int[][] partIdx = new int[parts][];
    float[][] partKey = new float[parts][];
    VectorEngine.run(parts, parts > 1, p -> {
      Top top = new Top(want);
      for (int i = (int) ((long) n * p / parts), hi = (int) ((long) n * (p + 1) / parts); i < hi; i++) {
        if ((i & 4095) == 0) VectorEngine.stopIfKilled();
        float key;
        if (binary) key = -hammingDistance(qb, bits[i]);
        else {
          float dot = dotCodes(query, codes[i]) * scale[i];
          key = switch (code) {
            case VectorEngine.COSINE -> norm[i] > 0 ? dot * fqInv / norm[i] : Float.NaN;
            case VectorEngine.EUCLIDEAN -> -(fq2 + norm[i] * norm[i] - 2 * dot);
            default -> dot;
          };
        }
        if (!Float.isNaN(key)) top.offer(key, i);
      }
      partIdx[p] = Arrays.copyOf(top.idx, top.size);
      partKey[p] = Arrays.copyOf(top.key, top.size);
    });
    Top all = new Top(want);
    for (int p = 0; p < parts; p++)
      for (int t = 0; t < partIdx[p].length; t++) all.offer(partKey[p][t], partIdx[p][t]);
    int c = all.size;
    if (hamming) {
      int[] index = Arrays.copyOf(all.idx, Math.min(k, c));
      float[] score = new float[index.length];
      for (int i = 0; i < index.length; i++) score[i] = -all.key[i];
      return result(index, score, true);
    }
    // Rescore the candidates: with the floats, or from the codes (int8: as scanned; binary: the
    // query's cosine with the vector of signs).
    Top best = new Top(Math.min(k, c));
    float[] lanes = new float[L];
    for (int t = 0; t < c; t++) {
      int i = all.idx[t];
      float key;
      if (floats != null) {
        float dot = VectorEngine.rowDot(query, floats[i], lanes);
        key = switch (code) {
          case VectorEngine.COSINE -> {
            float inv = VectorEngine.inverseNorm(floats[i], lanes);
            yield dot * fqInv * inv;
          }
          case VectorEngine.EUCLIDEAN -> -VectorEngine.rowDistanceSquared(query, floats[i], lanes);
          default -> dot;
        };
      } else if (binary) {
        key = signedSum(query, bits[i]) * fqInv / norm[i];
      } else key = all.key[t];
      if (!Float.isNaN(key)) best.offer(key, i);
    }
    float[] score = new float[best.size];
    for (int t = 0; t < best.size; t++)
      score[t] = code == VectorEngine.EUCLIDEAN ? (float) Math.sqrt(Math.max(0, -best.key[t])) : best.key[t];
    return result(Arrays.copyOf(best.idx, best.size), score, code == VectorEngine.EUCLIDEAN);
  }

  /** {@link #nearest(float[], int, String)} for a JavaScript or Python query. */
  public Matches nearest(Object query, int k, String metric) {
    return nearest(GuestVectors.vector(query, "the query"), k, metric);
  }

  public Matches nearest(Object query, int k, String metric, int oversample) {
    return nearest(GuestVectors.vector(query, "the query"), k, metric, oversample);
  }

  private Matches result(int[] index, float[] score, boolean distance) {
    long[] id = new long[index.length];
    for (int i = 0; i < index.length; i++) id[i] = ids[index[i]];
    return new Matches(index, score, id, distance);
  }

  /** The float query's dot product with int8 codes (to be multiplied by the vector's scale). */
  static float dotCodes(float[] q, byte[] c) {
    int d = q.length, i = 0;
    var acc = FloatVector.zero(SP);
    for (; i <= d - L; i += L) {
      var f = (FloatVector) ByteVector.fromArray(BS, c, i).convertShape(VectorOperators.B2F, SP, 0);
      acc = FloatVector.fromArray(SP, q, i).fma(f, acc);
    }
    float s = acc.reduceLanes(VectorOperators.ADD);
    for (; i < d; i++) s += q[i] * c[i];
    return s;
  }

  static int hammingDistance(long[] a, long[] b) {
    int c = 0;
    for (int i = 0; i < a.length; i++) c += Long.bitCount(a[i] ^ b[i]);
    return c;
  }

  /** The query's dot product with the vector of signs (+1 for a set bit, -1 otherwise). */
  private float signedSum(float[] q, long[] b) {
    float s = 0;
    for (int j = 0; j < d; j++) s += (b[j >>> 6] >>> (j & 63) & 1) != 0 ? q[j] : -q[j];
    return s;
  }

  /** The best `cap` (key, index) pairs: key descending, then index ascending (a total order). */
  private static final class Top {
    final float[] key;
    final int[] idx;
    final int cap;
    int size;

    Top(int cap) {
      this.cap = cap;
      key = new float[Math.max(cap, 0)];
      idx = new int[Math.max(cap, 0)];
    }

    void offer(float k, int i) {
      if (cap == 0) return;
      if (size == cap && !(k > key[size - 1] || (k == key[size - 1] && i < idx[size - 1]))) return;
      int at = size == cap ? cap - 1 : size++;
      while (at > 0 && (key[at - 1] < k || (key[at - 1] == k && idx[at - 1] > i))) {
        key[at] = key[at - 1];
        idx[at] = idx[at - 1];
        at--;
      }
      key[at] = k;
      idx[at] = i;
    }
  }

  @Override
  public String toString() {
    return "QuantizedSet(" + size() + " vectors of " + d + " dimensions, " + type()
        + (floats != null ? ", rescoring with the floats" : "") + ", " + bytes() / 1024 + " KiB of codes)";
  }
}
