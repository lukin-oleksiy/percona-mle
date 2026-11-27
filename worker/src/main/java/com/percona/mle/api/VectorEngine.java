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

import com.percona.mle.worker.SqlBridge;
import java.util.Arrays;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.IntConsumer;
import jdk.incubator.vector.FloatVector;
import jdk.incubator.vector.VectorSpecies;

/**
 * The kernels and threads of the vector helper (exp notes 31-33). Not API: routines see {@link
 * Vectors}, {@link VectorSet} and {@link Matches}.
 *
 * <p>One query: each row's dot product (and its squared norm when the metric needs it) with SIMD,
 * the top k kept on the fly; memory-bound, so rows are split over threads only when there are
 * enough of them. Several queries (and all pairs): BLAS-style register tiling. The rows are packed
 * k-major into panels of NR = 3 vectors of columns, one chunk of rows at a time (bounded memory);
 * a 4 x NR tile of scores is held in 12 vector accumulators (per element: 3 loads, 4 broadcasts,
 * 12 FMAs), and each task takes a block of queries through all panels of the chunk.
 *
 * <p>Results do not depend on the number of threads or on the JIT: each query's candidates are
 * offered in row order, a candidate equal to the k-th best does not displace it, and sums run in a
 * fixed order. The two kernels sum in different orders, so one query and a batch may differ in
 * near-ties.
 *
 * <p>Threads: one pool per worker ("mle-vectors-N", daemon, idle ones end after a minute), at most
 * mle.vector_threads (-Dmle.vectorThreads), never more than the CPUs. It only runs this class's
 * kernels, never guest code, so the worker's thread watch counts them as the runtime's. KILL: the
 * waiting thread sees the call killed (or its interrupt) and stops the tasks that have not
 * started; running tasks check between panels (and a scan every 4096 rows), so the call stops
 * within milliseconds and throws.
 *
 * <p>The species is a static final field and used directly: the JIT intrinsifies the Vector API
 * only for a constant species (a captured local made a kernel 16x slower, note 31).
 *
 * <p>The worker's JVM options keep tile*(), row*() and offer*() from being inlined (host/defaults.cc,
 * CompileCommand dontinline): smaller compilation units reach compiled code sooner in a new
 * worker. Renaming them means changing those options too.
 */
final class VectorEngine {
  private VectorEngine() {}

  static final VectorSpecies<Float> SP = FloatVector.SPECIES_PREFERRED;
  static final int L = SP.length();
  /** Tile columns: 3 vectors. */
  static final int NR = 3 * L;
  /** Rows packed at a time (whole panels): ~25 MB at 768 dimensions. */
  static final int CHUNK = Math.max(NR, Integer.getInteger("mle.vectorChunkRows", 8192) / NR * NR);
  /** Multiply-adds below which a single query is not split over threads (handing over costs ~50 µs). */
  static final long PARALLEL_MIN = Math.max(1, Long.getLong("mle.vectorParallelMin", 1L << 21));
  /** Rows of one query scanned between two checks for KILL on the calling thread. */
  private static final int SLICE = 32768;
  static final int THREADS = Math.max(1,
      Math.min(Integer.getInteger("mle.vectorThreads", 4), Runtime.getRuntime().availableProcessors()));

  private static volatile ThreadPoolExecutor pool;

  // ------------------------------------------------------------------ metrics

  static final int DOT = 0, COSINE = 1, EUCLIDEAN = 2;

  static int code(Vectors.Metric m) {
    if (m == null) throw new IllegalArgumentException("the metric must not be null (COSINE, DOT or EUCLIDEAN)");
    return switch (m) {
      case DOT -> DOT;
      case COSINE -> COSINE;
      case EUCLIDEAN -> EUCLIDEAN;
    };
  }

  // ---------------------------------------------------------- checks, norms

  /** The rows' common dimension; IllegalArgumentException for null or ragged rows. */
  static int dimension(float[][] rows) {
    if (rows == null) throw new IllegalArgumentException("rows must not be null");
    if (rows.length == 0) return 0;
    if (rows[0] == null) throw new IllegalArgumentException("row 0 is null");
    int d = rows[0].length;
    for (int i = 1; i < rows.length; i++) {
      if (rows[i] == null) throw new IllegalArgumentException("row " + i + " is null");
      if (rows[i].length != d)
        throw new IllegalArgumentException("row " + i + " has " + rows[i].length + " dimensions, row 0 has " + d);
    }
    return d;
  }

  /** q against the rows' dimension (rows as checked by dimension(); no rows: any query). */
  static void checkQuery(float[] q, float[][] rows, String what) {
    if (q == null) throw new IllegalArgumentException(what + " must not be null");
    if (rows.length > 0 && q.length != rows[0].length)
      throw new IllegalArgumentException(what + " has " + q.length + " dimensions, the rows have " + rows[0].length);
  }

  static void checkK(int k) {
    if (k < 1) throw new IllegalArgumentException("k must be at least 1, not " + k);
  }

  static float dot(float[] a, float[] b) {
    return rowDot(a, b, new float[L]);
  }

  /**
   * a·b (lanes: scratch of L floats). With rowDotSquare, the one-query kernel; small on purpose
   * and not inlined (worker JVM options), so a new worker compiles it within milliseconds.
   */
  static float rowDot(float[] a, float[] b, float[] lanes) {
    int d = a.length, i = 0;
    var s0 = FloatVector.zero(SP);
    var s1 = s0;
    for (int end = d - d % (2 * L); i < end; i += 2 * L) {
      s0 = FloatVector.fromArray(SP, a, i).fma(FloatVector.fromArray(SP, b, i), s0);
      s1 = FloatVector.fromArray(SP, a, i + L).fma(FloatVector.fromArray(SP, b, i + L), s1);
    }
    float s = sum(s0.add(s1), lanes);
    for (; i < d; i++) s += a[i] * b[i];
    return s;
  }

  /** q·r, and r·r into square[0], in one pass over r. */
  static float rowDotSquare(float[] q, float[] r, float[] lanes, float[] square) {
    int d = q.length, i = 0;
    var a0 = FloatVector.zero(SP);
    var a1 = a0;
    var b0 = a0;
    var b1 = a0;
    for (int end = d - d % (2 * L); i < end; i += 2 * L) {
      var r0 = FloatVector.fromArray(SP, r, i);
      var r1 = FloatVector.fromArray(SP, r, i + L);
      a0 = FloatVector.fromArray(SP, q, i).fma(r0, a0);
      a1 = FloatVector.fromArray(SP, q, i + L).fma(r1, a1);
      b0 = r0.fma(r0, b0);
      b1 = r1.fma(r1, b1);
    }
    float dot = sum(a0.add(a1), lanes);
    float rr = sum(b0.add(b1), lanes);
    for (; i < d; i++) {
      dot += q[i] * r[i];
      rr += r[i] * r[i];
    }
    square[0] = rr;
    return dot;
  }

  /**
   * |q - r|², summed directly. EUCLIDEAN ranks by it: |q|² + |r|² - 2q·r loses the difference
   * between close vectors far from the origin (a common offset), and so would pick the wrong rows.
   * Summed in float (the kernel's speed): differences above about 1.8e19 give Infinity, so rows
   * that far away tie (documented in Vectors).
   */
  static float rowDistanceSquared(float[] q, float[] r, float[] lanes) {
    int d = q.length, i = 0;
    var a0 = FloatVector.zero(SP);
    var a1 = a0;
    for (int end = d - d % (2 * L); i < end; i += 2 * L) {
      var x0 = FloatVector.fromArray(SP, q, i).sub(FloatVector.fromArray(SP, r, i));
      var x1 = FloatVector.fromArray(SP, q, i + L).sub(FloatVector.fromArray(SP, r, i + L));
      a0 = x0.fma(x0, a0);
      a1 = x1.fma(x1, a1);
    }
    float s = sum(a0.add(a1), lanes);
    for (; i < d; i++) {
      float x = q[i] - r[i];
      s += x * x;
    }
    return s;
  }

  /** Euclidean distance, summed directly, in float: Infinity for differences above about 1.8e19. */
  static float distance(float[] a, float[] b) {
    int d = a.length, i = 0;
    var s0 = FloatVector.zero(SP);
    for (int end = d - d % L; i < end; i += L) {
      var x = FloatVector.fromArray(SP, a, i).sub(FloatVector.fromArray(SP, b, i));
      s0 = x.fma(x, s0);
    }
    float s = sum(s0, new float[L]);
    for (; i < d; i++) {
      float x = a[i] - b[i];
      s += x * x;
    }
    return (float) Math.sqrt(s);
  }

  /**
   * The lanes' sum in a fixed order. Not reduceLanes(ADD): its order is unspecified, and the
   * interpreter and the compiled code differ in the last bit, so results would depend on the JIT.
   */
  static float sum(FloatVector v, float[] lanes) {
    v.intoArray(lanes, 0);
    float s = 0;
    for (int i = 0; i < lanes.length; i++) s += lanes[i];
    return s;
  }

  /**
   * 1 / |v|, 0 for a zero vector (lanes: scratch of L floats). |v|² is summed in float (the
   * kernel's speed), so 0 also when it underflows (every component below about 1e-19) or
   * overflows (a component above about 1.8e19): documented in Vectors.
   */
  static float inverseNorm(float[] v, float[] lanes) {
    float n = rowDot(v, v, lanes);
    return n > 0 ? (float) (1 / Math.sqrt(n)) : 0f;
  }

  /** Per row 1/|r| for COSINE; null for the other metrics. */
  static float[] rowTerms(float[][] rows, int metric) {
    if (metric != COSINE) return null;
    final int n = rows.length;
    final float[] t = new float[n];
    final int parts = parts((long) n * (rows.length == 0 ? 0 : rows[0].length), n);
    run(parts, parts > 1, p -> {
      final float[] lanes = new float[L];
      int lo = (int) ((long) n * p / parts), hi = (int) ((long) n * (p + 1) / parts);
      for (int j = lo; j < hi; j++) t[j] = inverseNorm(rows[j], lanes);
    });
    return t;
  }

  // ------------------------------------------------------------- one query

  /**
   * Top k of rows for one query. rowTerm: 1/|r| per row for COSINE (from a VectorSet), or null to
   * compute it with the dot product. self: a row to leave out, or -1. Keys: the dot product, the
   * cosine, or -|q - r|² (higher is better for all three).
   */
  static Matches nearest(float[] q, float[][] rows, float[] rowTerm, int k, int metric, int self, long[] ids) {
    final int n = rows.length;
    k = Math.min(k, Math.max(1, n));
    final float qInv = metric == COSINE ? inverseNorm(q, new float[L]) : 0f;
    final float[] s = new float[k];
    final int[] x = new int[k];
    Arrays.fill(s, Float.NEGATIVE_INFINITY);
    Arrays.fill(x, -1);
    final int d = q.length;
    final int parts = parts((long) n * d, n);
    if (parts > 1) {
      final float[][] ps = new float[parts][k];
      final int[][] px = new int[parts][k];
      final int kk = k;
      run(parts, true, p -> {
        Arrays.fill(ps[p], Float.NEGATIVE_INFINITY);
        Arrays.fill(px[p], -1);
        scan(q, rows, rowTerm, (int) ((long) n * p / parts), (int) ((long) n * (p + 1) / parts), self, metric,
            qInv, ps[p], px[p], kk);
      });
      for (int p = 0; p < parts; p++) {  // in row order: the result does not depend on the parts
        for (int c = 0; c < k && px[p][c] >= 0; c++) {
          if ((c & 4095) == 0) stopIfKilled();  // a large k: KILL is seen here too (MVP review API-2)
          offer(s, x, ps[p][c], px[p][c]);
        }
      }
    } else {
      final int slices = (n + SLICE - 1) / SLICE, kk = k;
      run(slices, false, sl -> scan(q, rows, rowTerm, sl * SLICE, Math.min(n, (sl + 1) * SLICE), self, metric, qInv,
          s, x, kk));
    }
    return matches(s, x, metric, ids);
  }

  /** Into s/x (descending keys): the rows [lo, hi) for query q; checks for KILL every 4096 rows. */
  private static void scan(float[] q, float[][] rows, float[] rowTerm, int lo, int hi, int self, int metric, float qInv,
      float[] s, int[] x, int k) {
    final boolean needSquare = metric == COSINE && rowTerm == null;
    final float[] lanes = new float[L], square = new float[1];
    for (int j = lo; j < hi; j++) {
      if ((j - lo & 4095) == 0) stopIfKilled();
      if (j == self) continue;
      final float key;
      if (metric == EUCLIDEAN) {
        key = -rowDistanceSquared(q, rows[j], lanes);
      } else {
        final float dot = needSquare ? rowDotSquare(q, rows[j], lanes, square) : rowDot(q, rows[j], lanes);
        final float rr = square[0];
        key = metric == COSINE
            ? dot * qInv * (rowTerm != null ? rowTerm[j] : rr > 0 ? (float) (1 / Math.sqrt(rr)) : 0f)
            : dot;
      }
      if (key >= s[k - 1]) offer(s, x, key, j);  // offer() decides ties; NaN never enters
    }
  }

  /**
   * Inserts (key, j) into the top k: higher key first, an equal key ordered by the lower row. The
   * order is total, so the result does not depend on the order of the offers (threads). An empty
   * slot (x = -1, key -Infinity) takes any key but NaN, -Infinity included.
   */
  private static void offer(float[] s, int[] x, float key, int j) {
    int k = s.length;
    if (!(key > s[k - 1] || key == s[k - 1] && (x[k - 1] < 0 || j < x[k - 1]))) return;
    if (k > OFFER_LINEAR_MAX) {
      offerLarge(s, x, key, j);
      return;
    }
    int p = k - 1;
    while (p > 0 && (s[p - 1] < key || s[p - 1] == key && (x[p - 1] < 0 || x[p - 1] > j))) {
      s[p] = s[p - 1];
      x[p] = x[p - 1];
      p--;
    }
    s[p] = key;
    x[p] = j;
  }

  /** Up to this k, offer() moves entries one by one; above, it searches and moves only filled ones. */
  private static final int OFFER_LINEAR_MAX = 64;

  /**
   * offer() for a large k: the filled slots (x >= 0) come first; a binary search finds the place
   * and only the filled slots after it move. The linear way moved the empty ones too: k²/2 moves
   * for the first k offers (MVP review API-2).
   */
  private static void offerLarge(float[] s, int[] x, float key, int j) {
    int k = s.length;
    int lo = 0;
    int hi = k;
    while (lo < hi) {  // filled = the first empty slot
      int mid = (lo + hi) >>> 1;
      if (x[mid] >= 0) lo = mid + 1;
      else hi = mid;
    }
    int filled = lo;
    lo = 0;
    hi = filled;
    while (lo < hi) {  // the first filled slot that (key, j) goes before
      int mid = (lo + hi) >>> 1;
      if (s[mid] > key || s[mid] == key && x[mid] < j) lo = mid + 1;
      else hi = mid;
    }
    int p = lo;
    int moved = Math.min(filled, k - 1) - p;
    if (moved > 0) {
      System.arraycopy(s, p, s, p + 1, moved);
      System.arraycopy(x, p, x, p + 1, moved);
    }
    s[p] = key;
    x[p] = j;
  }

  /** At most this many slots of top-k results for a batch (scores and rows): about 200 MB. */
  static final long TOP_K_MAX = 25_000_000;

  // ------------------------------------------------------- several queries

  /**
   * Top k of rows for each query. related: queries are the rows themselves, each row left out of
   * its own result (related(), each pair once). Fewer than 4 queries: one query at a time.
   */
  static Matches[] nearest(float[][] queries, float[][] rows, float[] rowTerm, int k, int metric, boolean related,
      long[] ids) {
    final int nq = queries.length, n = rows.length;
    final Matches[] out = new Matches[nq];
    if (nq < 4 || n == 0) {
      for (int a = 0; a < nq; a++) out[a] = nearest(queries[a], rows, rowTerm, k, metric, related ? a : -1, ids);
      return out;
    }
    final int d = rows[0].length, kk = Math.min(k, n);
    // The top k of every query is held at once (MVP review API-2: related() with a large k asked
    // for n x k arrays without a bound).
    if ((long) (related ? n : nq) * kk > TOP_K_MAX)
      throw new IllegalArgumentException((related ? n : nq) + " x " + kk + " best matches is more than " + TOP_K_MAX
          + ": ask for fewer (k) or fewer queries at once");
    final float[] rt = rowTerm != null ? rowTerm : rowTerms(rows, metric);
    if (related) return related(rows, rt, kk, metric, ids);
    final float[] qInv = new float[nq], lanes = new float[L];
    if (metric == COSINE)
      for (int a = 0; a < nq; a++) qInv[a] = inverseNorm(queries[a], lanes);
    final float[][] bs = new float[nq][kk];
    final int[][] bx = new int[nq][kk];
    for (int a = 0; a < nq; a++) {
      Arrays.fill(bs[a], Float.NEGATIVE_INFINITY);
      Arrays.fill(bx[a], -1);
    }
    // Query blocks: 128 queries (a panel stays in L2 for them), fewer when that leaves threads idle.
    final int rb = Math.max(4, Math.min(128, (nq + 2 * THREADS - 1) / (2 * THREADS) + 3 & ~3));
    final int tasks = (nq + rb - 1) / rb;
    final boolean parallel = (long) nq * n * d >= PARALLEL_MIN;
    final float[][] pk = new float[(Math.min(n, CHUNK) + NR - 1) / NR][d * NR];
    final float[] zero = new float[d];
    for (int c0 = 0; c0 < n; c0 += CHUNK) {
      final int c1 = Math.min(n, c0 + CHUNK), panels = (c1 - c0 + NR - 1) / NR, start = c0;
      run(panels, parallel, p -> pack(rows, start, c1, p, pk[p], d));
      run(tasks, parallel, t -> {
        final float[] tile = new float[4 * NR];
        final int lo = t * rb, hi = Math.min(nq, lo + rb);
        for (int p = 0; p < panels; p++) {
          stopIfKilled();
          final float[] panel = pk[p];
          final int j0 = start + p * NR, cols = Math.min(NR, c1 - j0);
          for (int a = lo; a < hi; a += 4) {
            if (metric == EUCLIDEAN)
              tileDistance(queries[a], a + 1 < hi ? queries[a + 1] : zero, a + 2 < hi ? queries[a + 2] : zero,
                  a + 3 < hi ? queries[a + 3] : zero, panel, d, tile);
            else
              tile(queries[a], a + 1 < hi ? queries[a + 1] : zero, a + 2 < hi ? queries[a + 2] : zero,
                  a + 3 < hi ? queries[a + 3] : zero, panel, d, tile);
            for (int r = 0; r < 4 && a + r < hi; r++) {
              final int qa = a + r;
              final float[] s = bs[qa];
              final int[] x = bx[qa];
              final float qi = qInv[qa];
              for (int c = 0; c < cols; c++) {
                final int j = j0 + c;
                final float v = tile[r * NR + c];
                final float key = switch (metric) {
                  case COSINE -> v * qi * rt[j];
                  case EUCLIDEAN -> -v;  // v = |q - r|²
                  default -> v;
                };
                if (key >= s[kk - 1]) offer(s, x, key, j);
              }
            }
          }
        }
      });
    }
    for (int a = 0; a < nq; a++) out[a] = matches(bs[a], bx[a], metric, ids);
    return out;
  }

  /**
   * All pairs, each computed once: a task takes a block of rows a and the rows j > a, and offers
   * the score to both rows. A row's top k then gets offers from several tasks: under its lock, taken
   * only when the score beats its k-th best (rare once it is full; the k-th best only rises, so the
   * test without the lock never rejects wrongly). The cosine key is dot * (1/|a| * 1/|j|) and the
   * EUCLIDEAN key -|a - j|², the same for both rows. Half the work of the general batch, with blocks
   * of rows in order of their work.
   */
  private static Matches[] related(float[][] rows, float[] rt, int kk, int metric, long[] ids) {
    final int n = rows.length;
    final float[][] bs = new float[n][kk];
    final int[][] bx = new int[n][kk];
    for (int a = 0; a < n; a++) {
      Arrays.fill(bs[a], Float.NEGATIVE_INFINITY);
      Arrays.fill(bx[a], -1);
    }
    upperTiles(rows, metric, t -> (tile, a, hi, j0, cols) -> {
      for (int r = 0; r < 4 && a + r < hi; r++) {
        final int qa = a + r;
        for (int c = Math.max(0, qa + 1 - j0); c < cols; c++) {
          final int j = j0 + c;
          final float v = tile[r * NR + c];
          final float key = switch (metric) {
            case COSINE -> v * (rt[qa] * rt[j]);
            case EUCLIDEAN -> -v;  // v = |a - j|²
            default -> v;
          };
          offerShared(bs[qa], bx[qa], key, j);
          offerShared(bs[j], bx[j], key, qa);
        }
      }
    });
    final Matches[] out = new Matches[n];
    for (int a = 0; a < n; a++) out[a] = matches(bs[a], bx[a], metric, ids);
    return out;
  }

  /** offer() into a top k that other threads offer to as well. */
  private static void offerShared(float[] s, int[] x, float key, int j) {
    if (key < s[s.length - 1]) return;  // the k-th best only rises: never wrong without the lock
    synchronized (x) {
      offer(s, x, key, j);
    }
  }

  /**
   * acc[i][j] += the dot product of m[i] and m[j] over their first len values, for j >= i (the
   * upper triangle): a Gram matrix m mᵀ, summed chunk by chunk into acc in double (PCA's
   * covariance: m holds a chunk of centred rows transposed). The same 4 x NR register tiles as the
   * batch search; each task owns a block of rows of acc, so the sums do not depend on the threads.
   */
  static void gramAdd(float[][] m, int len, double[][] acc) {
    final int k = m.length;
    if (k == 0 || len == 0) return;
    final int panels = (k + NR - 1) / NR;
    final float[][] pk = new float[panels][len * NR];
    final boolean parallel = (long) k * k * len / 2 >= PARALLEL_MIN;
    run(panels, parallel, p -> pack(m, 0, k, p, pk[p], len));
    final float[] zero = new float[len];
    final int rb = 16;  // rows of acc per task
    final int tasks = (k + rb - 1) / rb;
    run(tasks, parallel, t -> {
      final float[] tile = new float[4 * NR];
      final int lo = t * rb, hi = Math.min(k, lo + rb);
      for (int p = lo / NR; p < panels; p++) {  // panels with columns j >= lo
        stopIfKilled();
        final int j0 = p * NR, cols = Math.min(NR, k - j0);
        for (int a = lo; a < hi; a += 4) {
          tile(m[a], a + 1 < hi ? m[a + 1] : zero, a + 2 < hi ? m[a + 2] : zero, a + 3 < hi ? m[a + 3] : zero, pk[p],
              len, tile);
          for (int r = 0; r < 4 && a + r < hi; r++) {
            final double[] row = acc[a + r];
            for (int c = Math.max(0, a + r - j0); c < cols; c++) row[j0 + c] += tile[r * NR + c];
          }
        }
      }
    });
  }

  /** Packs rows [c0 + p*NR, ...) of the chunk ending at c1 into panel p (k-major; missing columns zero). */
  private static void pack(float[][] rows, int c0, int c1, int p, float[] panel, int d) {
    final int j0 = c0 + p * NR, cols = Math.min(NR, c1 - j0);
    if (cols < NR) Arrays.fill(panel, 0f);
    for (int c = 0; c < cols; c++) {
      final float[] row = rows[j0 + c];
      for (int i = 0, o = c; i < d; i++, o += NR) panel[o] = row[i];
    }
  }

  /** The 4 x NR dot products of queries a0..a3 with a packed panel, into tile (row-major). */
  private static void tile(float[] a0, float[] a1, float[] a2, float[] a3, float[] q, int d, float[] tile) {
    var c00 = FloatVector.zero(SP);
    var c01 = c00;
    var c02 = c00;
    var c10 = c00;
    var c11 = c00;
    var c12 = c00;
    var c20 = c00;
    var c21 = c00;
    var c22 = c00;
    var c30 = c00;
    var c31 = c00;
    var c32 = c00;
    for (int i = 0, o = 0; i < d; i++, o += NR) {
      var b0 = FloatVector.fromArray(SP, q, o);
      var b1 = FloatVector.fromArray(SP, q, o + L);
      var b2 = FloatVector.fromArray(SP, q, o + 2 * L);
      var x = FloatVector.broadcast(SP, a0[i]);
      c00 = b0.fma(x, c00);
      c01 = b1.fma(x, c01);
      c02 = b2.fma(x, c02);
      x = FloatVector.broadcast(SP, a1[i]);
      c10 = b0.fma(x, c10);
      c11 = b1.fma(x, c11);
      c12 = b2.fma(x, c12);
      x = FloatVector.broadcast(SP, a2[i]);
      c20 = b0.fma(x, c20);
      c21 = b1.fma(x, c21);
      c22 = b2.fma(x, c22);
      x = FloatVector.broadcast(SP, a3[i]);
      c30 = b0.fma(x, c30);
      c31 = b1.fma(x, c31);
      c32 = b2.fma(x, c32);
    }
    c00.intoArray(tile, 0);
    c01.intoArray(tile, L);
    c02.intoArray(tile, 2 * L);
    c10.intoArray(tile, NR);
    c11.intoArray(tile, NR + L);
    c12.intoArray(tile, NR + 2 * L);
    c20.intoArray(tile, 2 * NR);
    c21.intoArray(tile, 2 * NR + L);
    c22.intoArray(tile, 2 * NR + 2 * L);
    c30.intoArray(tile, 3 * NR);
    c31.intoArray(tile, 3 * NR + L);
    c32.intoArray(tile, 3 * NR + 2 * L);
  }

  /** The 4 x NR squared distances |a - b|² of queries a0..a3 to a packed panel (as tile()). */
  private static void tileDistance(float[] a0, float[] a1, float[] a2, float[] a3, float[] q, int d,
      float[] tile) {
    var c00 = FloatVector.zero(SP);
    var c01 = c00;
    var c02 = c00;
    var c10 = c00;
    var c11 = c00;
    var c12 = c00;
    var c20 = c00;
    var c21 = c00;
    var c22 = c00;
    var c30 = c00;
    var c31 = c00;
    var c32 = c00;
    for (int i = 0, o = 0; i < d; i++, o += NR) {
      var b0 = FloatVector.fromArray(SP, q, o);
      var b1 = FloatVector.fromArray(SP, q, o + L);
      var b2 = FloatVector.fromArray(SP, q, o + 2 * L);
      var x = FloatVector.broadcast(SP, a0[i]);
      var y0 = b0.sub(x);
      var y1 = b1.sub(x);
      var y2 = b2.sub(x);
      c00 = y0.fma(y0, c00);
      c01 = y1.fma(y1, c01);
      c02 = y2.fma(y2, c02);
      x = FloatVector.broadcast(SP, a1[i]);
      y0 = b0.sub(x);
      y1 = b1.sub(x);
      y2 = b2.sub(x);
      c10 = y0.fma(y0, c10);
      c11 = y1.fma(y1, c11);
      c12 = y2.fma(y2, c12);
      x = FloatVector.broadcast(SP, a2[i]);
      y0 = b0.sub(x);
      y1 = b1.sub(x);
      y2 = b2.sub(x);
      c20 = y0.fma(y0, c20);
      c21 = y1.fma(y1, c21);
      c22 = y2.fma(y2, c22);
      x = FloatVector.broadcast(SP, a3[i]);
      y0 = b0.sub(x);
      y1 = b1.sub(x);
      y2 = b2.sub(x);
      c30 = y0.fma(y0, c30);
      c31 = y1.fma(y1, c31);
      c32 = y2.fma(y2, c32);
    }
    c00.intoArray(tile, 0);
    c01.intoArray(tile, L);
    c02.intoArray(tile, 2 * L);
    c10.intoArray(tile, NR);
    c11.intoArray(tile, NR + L);
    c12.intoArray(tile, NR + 2 * L);
    c20.intoArray(tile, 2 * NR);
    c21.intoArray(tile, 2 * NR + L);
    c22.intoArray(tile, 2 * NR + 2 * L);
    c30.intoArray(tile, 3 * NR);
    c31.intoArray(tile, 3 * NR + L);
    c32.intoArray(tile, 3 * NR + 2 * L);
  }

  /** The result: the scores of the keys (EUCLIDEAN: the distance, sqrt(-key); ascending). */
  private static Matches matches(float[] s, int[] x, int metric, long[] ids) {
    int m = 0;
    while (m < x.length && x[m] >= 0) m++;
    int[] index = Arrays.copyOf(x, m);
    float[] score = Arrays.copyOf(s, m);
    if (metric == EUCLIDEAN)
      for (int i = 0; i < m; i++) score[i] = (float) Math.sqrt(-score[i]);
    long[] id = null;
    if (ids != null) {
      id = new long[m];
      for (int i = 0; i < m; i++) id[i] = ids[index[i]];
    }
    return new Matches(index, score, id, metric == EUCLIDEAN);
  }

  // ------------------------------------------------ every score (exp note 53)

  /** The score of a key: the distance for EUCLIDEAN (key -|q - r|²), the key itself otherwise. */
  static float score(float key, int metric) {
    return metric == EUCLIDEAN ? (float) Math.sqrt(-key) : key;
  }

  /**
   * The key of a threshold in the metric's unit: a similarity, or a distance for EUCLIDEAN. Keys
   * are floats and a row matches with key >= this one, so the threshold is rounded up to float:
   * rounded to the nearest it could fall below the threshold and let in a score less than it.
   */
  static float keyOf(double threshold, int metric) {
    if (Double.isNaN(threshold)) throw new IllegalArgumentException("the threshold must be a number, not NaN");
    double key = threshold;
    if (metric == EUCLIDEAN) {
      if (!(threshold >= 0))
        throw new IllegalArgumentException("a distance threshold must be at least 0, not " + threshold);
      key = -(threshold * threshold);
    }
    float f = (float) key;
    // -Infinity stays: a threshold below every float lets every row in, as it did.
    return f < key && f != Float.NEGATIVE_INFINITY ? Math.nextUp(f) : f;
  }

  /** Every row's key for query q (higher is better), split over threads like nearest(). */
  static float[] keys(float[] q, float[][] rows, float[] rowTerm, int metric) {
    final int n = rows.length;
    final float[] out = new float[n];
    final float qInv = metric == COSINE ? inverseNorm(q, new float[L]) : 0f;
    final int parts = parts((long) n * q.length, n);
    if (parts > 1)
      run(parts, true, p -> keys(q, rows, rowTerm, metric, qInv, (int) ((long) n * p / parts),
          (int) ((long) n * (p + 1) / parts), out));
    else
      run((n + SLICE - 1) / SLICE, false, sl -> keys(q, rows, rowTerm, metric, qInv, sl * SLICE,
          Math.min(n, (sl + 1) * SLICE), out));
    return out;
  }

  /** Keys of rows [lo, hi) for q into out; checks for KILL every 4096 rows. */
  static void keys(float[] q, float[][] rows, float[] rowTerm, int metric, float qInv, int lo, int hi, float[] out) {
    final float[] lanes = new float[L], square = new float[1];
    for (int j = lo; j < hi; j++) {
      if ((j - lo & 4095) == 0) stopIfKilled();
      if (metric == EUCLIDEAN) {
        out[j] = -rowDistanceSquared(q, rows[j], lanes);
      } else if (metric == DOT) {
        out[j] = rowDot(q, rows[j], lanes);
      } else if (rowTerm != null) {
        out[j] = rowDot(q, rows[j], lanes) * qInv * rowTerm[j];
      } else {
        float dot = rowDotSquare(q, rows[j], lanes, square);
        out[j] = dot * qInv * (square[0] > 0 ? (float) (1 / Math.sqrt(square[0])) : 0f);
      }
    }
  }

  /** Receives pairs (a < b) found by {@link #pairs}; called under the sink's lock. */
  interface PairSink {
    /** Takes count pairs; returns the key below which no more pairs are wanted (it only rises). */
    float take(int[] a, int[] b, float[] key, int count);
  }

  /**
   * Every pair a < b of rows with key >= keyMin, to the sink: the related() kernel (each pair once,
   * register tiles, threads), each task buffering its pairs. The pairs reach the sink in an order
   * that depends on the threads; sinks give a result that does not (a total order, or components).
   */
  static void pairs(float[][] rows, float[] rowTerm, int metric, float keyMin, PairSink sink) {
    final int n = rows.length;
    if (n < 2) return;
    final float[] rt = rowTerm != null ? rowTerm : rowTerms(rows, metric);
    final float[] cut = {keyMin};  // under the sink's lock; read racily (it only rises)
    upperTiles(rows, metric, t -> new PairTask(rt, metric, sink, cut));
  }

  /** One task's consumer of the register tiles of {@link #upperTiles}. */
  interface UpperTile {
    /**
     * tile[r * NR + c] is the dot (EUCLIDEAN: the squared distance) of rows a + r (below hi) and
     * j0 + c (below cols); only the pairs with j > a + r are new.
     */
    void take(float[] tile, int a, int hi, int j0, int cols);

    /** After the task's last tile. */
    default void end() {}
  }

  /**
   * The upper triangle of all pairs, each computed once (related() and pairs(), MVP review API-8):
   * a task takes a block of rows a and the rows j > a in register tiles, chunk by chunk of packed
   * panels; the first blocks have the most rows j > a, small enough to balance the threads. Each
   * task gets its consumer from perTask; one call per tile keeps the per-pair code in it.
   */
  private static void upperTiles(float[][] rows, int metric, java.util.function.IntFunction<UpperTile> perTask) {
    final int n = rows.length, d = rows[0].length;
    final int rb = Math.max(4, Math.min(128, (n + 4 * THREADS - 1) / (4 * THREADS) + 3 & ~3));
    final boolean parallel = (long) n * n / 2 * d >= PARALLEL_MIN;
    final float[][] pk = new float[(Math.min(n, CHUNK) + NR - 1) / NR][d * NR];
    final float[] zero = new float[d];
    for (int c0 = 0; c0 < n; c0 += CHUNK) {
      final int c1 = Math.min(n, c0 + CHUNK), panels = (c1 - c0 + NR - 1) / NR, start = c0;
      // Blocks with a row below the chunk's last row have pairs in it.
      final int tasks = Math.min((n + rb - 1) / rb, (c1 - 1 + rb - 1) / rb);
      if (tasks == 0) continue;
      run(panels, parallel, p -> pack(rows, start, c1, p, pk[p], d));
      run(tasks, parallel, t -> {
        final UpperTile consumer = perTask.apply(t);
        final float[] tile = new float[4 * NR];
        final int lo = t * rb, hi = Math.min(n, lo + rb);
        for (int p = Math.max(0, (lo + 1 - start) / NR); p < panels; p++) {
          stopIfKilled();
          final float[] panel = pk[p];
          final int j0 = start + p * NR, cols = Math.min(NR, c1 - j0);
          for (int a = lo; a < hi; a += 4) {
            if (j0 + cols - 1 <= a) break;  // no j > a here, nor for the later rows
            if (metric == EUCLIDEAN)
              tileDistance(rows[a], a + 1 < hi ? rows[a + 1] : zero, a + 2 < hi ? rows[a + 2] : zero,
                  a + 3 < hi ? rows[a + 3] : zero, panel, d, tile);
            else
              tile(rows[a], a + 1 < hi ? rows[a + 1] : zero, a + 2 < hi ? rows[a + 2] : zero,
                  a + 3 < hi ? rows[a + 3] : zero, panel, d, tile);
            consumer.take(tile, a, hi, j0, cols);
          }
        }
        consumer.end();
      });
    }
  }

  /** A pairs() task: buffers the pairs at least the sink's cut and hands them over 8192 at a time. */
  private static final class PairTask implements UpperTile {
    private static final int CAP = 8192;
    private final float[] rt;
    private final int metric;
    private final PairSink sink;
    private final float[] cut;
    private final int[] ba = new int[CAP], bb = new int[CAP];
    private final float[] bk = new float[CAP];
    private int m;
    private float min;

    PairTask(float[] rt, int metric, PairSink sink, float[] cut) {
      this.rt = rt;
      this.metric = metric;
      this.sink = sink;
      this.cut = cut;
      synchronized (sink) {
        min = cut[0];
      }
    }

    @Override
    public void take(float[] tile, int a, int hi, int j0, int cols) {
      for (int r = 0; r < 4 && a + r < hi; r++) {
        final int qa = a + r;
        for (int c = Math.max(0, qa + 1 - j0); c < cols; c++) {
          final int j = j0 + c;
          final float v = tile[r * NR + c];
          final float key = switch (metric) {
            case COSINE -> v * (rt[qa] * rt[j]);
            case EUCLIDEAN -> -v;
            default -> v;
          };
          if (key >= min) {
            ba[m] = qa;
            bb[m] = j;
            bk[m] = key;
            if (++m == CAP) flush();
          }
        }
      }
    }

    @Override
    public void end() {
      if (m > 0) flush();
    }

    private void flush() {
      synchronized (sink) {
        cut[0] = Math.max(cut[0], sink.take(ba, bb, bk, m));
        min = cut[0];
      }
      m = 0;
    }
  }

  // ---------------------------------------------------------------- threads

  /** How many parts work of this size is split into (1: on the calling thread). */
  static int parts(long work, int n) {
    if (THREADS <= 1 || work < 2 * PARALLEL_MIN) return 1;
    return (int) Math.max(1, Math.min(Math.min(THREADS, work / PARALLEL_MIN), n));
  }

  /**
   * body(0..tasks-1): on the pool when parallel, else on this thread. A killed call stops between
   * tasks with CancellationException; a task's exception or error is rethrown here.
   */
  static void run(int tasks, boolean parallel, IntConsumer body) {
    if (!parallel || tasks <= 1 || THREADS <= 1) {
      for (int t = 0; t < tasks; t++) {
        if (killed()) throw cancelled();
        body.accept(t);
      }
      return;
    }
    Job job = new Job(tasks);
    ThreadPoolExecutor p = pool();
    for (int t = 0; t < tasks; t++) {
      final int tt = t;
      p.execute(() -> job.run(tt, body));
    }
    job.await();
  }

  /** Inside a task (on the pool or the calling thread): a killed call stops here. */
  static void stopIfKilled() {
    if (killed()) throw cancelled();
  }

  private static boolean killed() {
    return Thread.currentThread().isInterrupted() || SqlBridge.killed();
  }

  private static CancellationException cancelled() {
    return new CancellationException("interrupted (the statement was killed)");
  }

  private static ThreadPoolExecutor pool() {
    ThreadPoolExecutor p = pool;
    if (p != null) return p;
    synchronized (VectorEngine.class) {
      if (pool == null) {
        AtomicInteger n = new AtomicInteger();
        ThreadPoolExecutor e = new ThreadPoolExecutor(THREADS, THREADS, 60, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(), r -> {
              Thread t = new Thread(r, "mle-vectors-" + n.incrementAndGet());
              t.setDaemon(true);
              return t;
            });
        e.allowCoreThreadTimeOut(true);
        pool = e;
      }
      return pool;
    }
  }

  private static final class Job {
    private final CountDownLatch done;
    private volatile boolean cancel;
    private volatile Throwable failure;

    Job(int tasks) {
      done = new CountDownLatch(tasks);
    }

    void run(int t, IntConsumer body) {
      try {
        if (!cancel) body.accept(t);
      } catch (Throwable e) {
        if (failure == null) failure = e;
        cancel = true;
      } finally {
        done.countDown();
      }
    }

    /** Waits for every task; on KILL, cancels the rest and still waits (tasks are short). */
    void await() {
      boolean killed = false, interrupted = false;
      while (true) {
        try {
          if (done.await(10, TimeUnit.MILLISECONDS)) break;
          if (!cancel && SqlBridge.killed()) {
            cancel = true;
            killed = true;
          }
        } catch (InterruptedException e) {
          interrupted = true;
          cancel = true;
          killed = true;
        }
      }
      if (interrupted) Thread.currentThread().interrupt();
      Throwable f = failure;
      if (f instanceof RuntimeException re) throw re;
      if (f instanceof Error er) throw er;
      if (f != null) throw new IllegalStateException(f);
      if (killed) throw cancelled();
    }
  }
}
