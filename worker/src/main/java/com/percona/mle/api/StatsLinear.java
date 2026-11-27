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

/**
 * Dense linear algebra in double for the statistics helper (exp note 75): least squares by a
 * blocked QR (TSQR), and the eigenvalues of a symmetric matrix (Householder tridiagonalization
 * and the implicit QL algorithm, as EISPACK's tred2 and tql2). Not API.
 *
 * <p>Threads: TSQR factors blocks of {@link #BLOCK} rows on the vector pool (each block's R alone)
 * and combines the R factors in block order on the calling thread, so the result does not depend
 * on the number of threads.
 */
final class StatsLinear {
  private StatsLinear() {}

  static final int BLOCK = 2048;

  /**
   * The R factor (m x m, upper triangular) of the n x m matrix whose column j is cols[j][rows[i]]
   * (rows: the rows to use; null: all). Columns are given as they are (an intercept is a column of ones).
   */
  static double[][] r(double[][] cols, int[] rows, int n) {
    final int m = cols.length;
    final int blocks = Math.max(1, (n + BLOCK - 1) / BLOCK);
    final double[][][] rs = new double[blocks][][];
    boolean parallel = VectorEngine.parts((long) n * m * m, blocks) > 1;
    VectorEngine.run(blocks, parallel, b -> {
      int lo = b * BLOCK, hi = Math.min(n, lo + BLOCK), len = hi - lo;
      double[][] a = new double[Math.max(len, m)][m];  // rows of this block, zero rows to at least m
      for (int i = 0; i < len; i++) {
        int row = rows == null ? lo + i : rows[lo + i];
        for (int j = 0; j < m; j++) a[i][j] = cols[j][row];
      }
      rs[b] = householderR(a);
    });
    double[][] r = rs[0];
    for (int b = 1; b < blocks; b++) {  // in block order: [R; R_b] -> R
      double[][] a = new double[2 * m][];
      for (int i = 0; i < m; i++) {
        a[i] = r[i];
        a[m + i] = rs[b][i];
      }
      r = householderR(a);
    }
    return r;
  }

  /** The upper m x m R of a (rows x m, rows >= m) by Householder reflections; a is overwritten. */
  static double[][] householderR(double[][] a) {
    int rows = a.length, m = a[0].length;
    for (int k = 0; k < m; k++) {
      double norm = 0;  // scaled to avoid overflow
      double scale = 0;
      for (int i = k; i < rows; i++) scale = Math.max(scale, Math.abs(a[i][k]));
      if (scale == 0) continue;
      for (int i = k; i < rows; i++) {
        double v = a[i][k] / scale;
        norm += v * v;
      }
      norm = Math.sqrt(norm) * scale;
      double alpha = a[k][k] > 0 ? -norm : norm;
      // v = x - alpha e1, stored in column k below the diagonal; beta = 2 / v'v
      double v0 = a[k][k] - alpha;
      double vtv = v0 * v0;
      for (int i = k + 1; i < rows; i++) vtv += a[i][k] * a[i][k];
      a[k][k] = alpha;
      if (vtv == 0) continue;
      for (int j = k + 1; j < m; j++) {
        double s = v0 * a[k][j];
        for (int i = k + 1; i < rows; i++) s += a[i][k] * a[i][j];
        s *= 2 / vtv;
        a[k][j] -= s * v0;
        for (int i = k + 1; i < rows; i++) a[i][j] -= s * a[i][k];
      }
    }
    double[][] r = new double[m][m];
    for (int i = 0; i < m; i++)
      for (int j = i; j < m; j++) r[i][j] = a[i][j];
    return r;
  }

  /** The inverse of an upper triangular matrix. */
  static double[][] inverseUpper(double[][] r, int p) {
    double[][] inv = new double[p][p];
    for (int j = p - 1; j >= 0; j--) {
      inv[j][j] = 1 / r[j][j];
      for (int i = j - 1; i >= 0; i--) {
        double s = 0;
        for (int k = i + 1; k <= j; k++) s += r[i][k] * inv[k][j];
        inv[i][j] = -s / r[i][i];
      }
    }
    return inv;
  }

  /**
   * Eigenvalues (ascending) and eigenvectors (columns of v) of a symmetric matrix a (n x n, not
   * changed): {values} in d, vectors in v.
   */
  static double[] symmetricEigen(double[][] a, double[][] v) {
    int n = a.length;
    for (int i = 0; i < n; i++) System.arraycopy(a[i], 0, v[i], 0, n);
    double[] d = new double[n], e = new double[n];
    tred2(n, v, d, e);
    tql2(n, v, d, e);
    return d;
  }

  /** Householder reduction to tridiagonal form (EISPACK tred2, as JAMA). */
  private static void tred2(int n, double[][] V, double[] d, double[] e) {
    for (int j = 0; j < n; j++) d[j] = V[n - 1][j];
    for (int i = n - 1; i > 0; i--) {
      double scale = 0, h = 0;
      for (int k = 0; k < i; k++) scale += Math.abs(d[k]);
      if (scale == 0) {
        e[i] = d[i - 1];
        for (int j = 0; j < i; j++) {
          d[j] = V[i - 1][j];
          V[i][j] = 0;
          V[j][i] = 0;
        }
      } else {
        for (int k = 0; k < i; k++) {
          d[k] /= scale;
          h += d[k] * d[k];
        }
        double f = d[i - 1], g = Math.sqrt(h);
        if (f > 0) g = -g;
        e[i] = scale * g;
        h -= f * g;
        d[i - 1] = f - g;
        for (int j = 0; j < i; j++) e[j] = 0;
        for (int j = 0; j < i; j++) {
          f = d[j];
          V[j][i] = f;
          g = e[j] + V[j][j] * f;
          for (int k = j + 1; k <= i - 1; k++) {
            g += V[k][j] * d[k];
            e[k] += V[k][j] * f;
          }
          e[j] = g;
        }
        f = 0;
        for (int j = 0; j < i; j++) {
          e[j] /= h;
          f += e[j] * d[j];
        }
        double hh = f / (h + h);
        for (int j = 0; j < i; j++) e[j] -= hh * d[j];
        for (int j = 0; j < i; j++) {
          f = d[j];
          g = e[j];
          for (int k = j; k <= i - 1; k++) V[k][j] -= (f * e[k] + g * d[k]);
          d[j] = V[i - 1][j];
          V[i][j] = 0;
        }
      }
      d[i] = h;
    }
    for (int i = 0; i < n - 1; i++) {
      V[n - 1][i] = V[i][i];
      V[i][i] = 1;
      double h = d[i + 1];
      if (h != 0) {
        for (int k = 0; k <= i; k++) d[k] = V[k][i + 1] / h;
        for (int j = 0; j <= i; j++) {
          double g = 0;
          for (int k = 0; k <= i; k++) g += V[k][i + 1] * V[k][j];
          for (int k = 0; k <= i; k++) V[k][j] -= g * d[k];
        }
      }
      for (int k = 0; k <= i; k++) V[k][i + 1] = 0;
    }
    for (int j = 0; j < n; j++) {
      d[j] = V[n - 1][j];
      V[n - 1][j] = 0;
    }
    V[n - 1][n - 1] = 1;
    e[0] = 0;
  }

  /** Implicit QL on the tridiagonal form (EISPACK tql2, as JAMA); eigenvalues sorted ascending. */
  private static void tql2(int n, double[][] V, double[] d, double[] e) {
    for (int i = 1; i < n; i++) e[i - 1] = e[i];
    e[n - 1] = 0;
    double f = 0, tst1 = 0, eps = Math.ulp(1.0);
    for (int l = 0; l < n; l++) {
      tst1 = Math.max(tst1, Math.abs(d[l]) + Math.abs(e[l]));
      int m = l;
      while (m < n) {
        if (Math.abs(e[m]) <= eps * tst1) break;
        m++;
      }
      if (m > l) {
        int iter = 0;
        do {
          if (++iter > 300) throw new ArithmeticException("the eigenvalues did not converge");
          if (iter % 32 == 0) VectorEngine.stopIfKilled();
          double g = d[l], p = (d[l + 1] - g) / (2 * e[l]), r = Math.hypot(p, 1);
          if (p < 0) r = -r;
          d[l] = e[l] / (p + r);
          d[l + 1] = e[l] * (p + r);
          double dl1 = d[l + 1], h = g - d[l];
          for (int i = l + 2; i < n; i++) d[i] -= h;
          f += h;
          p = d[m];
          double c = 1, c2 = c, c3 = c, el1 = e[l + 1], s = 0, s2 = 0;
          for (int i = m - 1; i >= l; i--) {
            c3 = c2;
            c2 = c;
            s2 = s;
            g = c * e[i];
            h = c * p;
            r = Math.hypot(p, e[i]);
            e[i + 1] = s * r;
            s = e[i] / r;
            c = p / r;
            p = c * d[i] - s * g;
            d[i + 1] = h + s * (c * g + s * d[i]);
            for (int k = 0; k < n; k++) {
              h = V[k][i + 1];
              V[k][i + 1] = s * V[k][i] + c * h;
              V[k][i] = c * V[k][i] - s * h;
            }
          }
          p = -s * s2 * c3 * el1 * e[l] / dl1;
          e[l] = s * p;
          d[l] = c * p;
        } while (Math.abs(e[l]) > eps * tst1);
      }
      d[l] += f;
      e[l] = 0;
    }
    for (int i = 0; i < n - 1; i++) {  // selection sort, ascending, vectors along
      int k = i;
      double p = d[i];
      for (int j = i + 1; j < n; j++)
        if (d[j] < p) {
          k = j;
          p = d[j];
        }
      if (k != i) {
        d[k] = d[i];
        d[i] = p;
        for (int j = 0; j < n; j++) {
          double t = V[j][i];
          V[j][i] = V[j][k];
          V[j][k] = t;
        }
      }
    }
  }
}
