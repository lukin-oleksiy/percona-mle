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
 * Principal component analysis ({@link Stats#pca}, {@link Frame#pca}, {@link Vectors#pca}, {@link
 * VectorSet#pca}), as scikit-learn's PCA: the data centred (not scaled), the k directions of the
 * largest variance (components, unit length, each with its largest coordinate positive, as
 * scikit-learn's sign rule), the variance along each (n - 1) and its share of the total. JSON with
 * {@code String(p)} / {@code str(p)}.
 *
 * <p>Columns of numbers (statistics): rows with a missing value are left out; the covariance comes
 * from a blocked QR in double. Vectors: the covariance is summed from chunks of rows on the vector
 * pool (SIMD tiles in float, chunks added in double, in order); then the eigenvectors of the
 * covariance (Householder and QL, in double). Results do not depend on the number of threads.
 */
public final class Pca {
  private final double[][] components;  // k x p
  private final double[] variance, ratio, mean;
  private final String[] names;
  private final long n;

  private Pca(double[][] components, double[] variance, double[] ratio, double[] mean, String[] names, long n) {
    this.components = components;
    this.variance = variance;
    this.ratio = ratio;
    this.mean = mean;
    this.names = names;
    this.n = n;
  }

  /** From the covariance matrix (p x p, upper triangle at least), the top k. */
  private static Pca of(double[][] cov, int k, double[] mean, String[] names, long n) {
    int p = cov.length;
    if (k < 1 || k > p) throw new IllegalArgumentException("k must be from 1 to " + p + ", not " + k);
    for (int i = 0; i < p; i++)
      for (int j = 0; j < i; j++) cov[i][j] = cov[j][i];
    double[][] v = new double[p][p];
    double[] d = StatsLinear.symmetricEigen(cov, v);
    double trace = 0;
    for (int i = 0; i < p; i++) trace += cov[i][i];
    double[][] comp = new double[k][p];
    double[] var = new double[k], ratio = new double[k];
    for (int c = 0; c < k; c++) {
      int col = p - 1 - c;  // descending
      var[c] = Math.max(0, d[col]);
      ratio[c] = trace > 0 ? var[c] / trace : Double.NaN;
      int big = 0;
      for (int i = 0; i < p; i++) {
        comp[c][i] = v[col][i];
        if (Math.abs(comp[c][i]) > Math.abs(comp[c][big])) big = i;
      }
      if (comp[c][big] < 0)
        for (int i = 0; i < p; i++) comp[c][i] = -comp[c][i];
    }
    return new Pca(comp, var, ratio, mean, names, n);
  }

  static Pca ofColumns(double[][] cols, int k, String[] names) {
    if (cols == null || cols.length == 0) throw new IllegalArgumentException("PCA needs at least one column");
    int p = cols.length, total = cols[0].length;
    for (int j = 0; j < p; j++)
      if (cols[j] == null || cols[j].length != total)
        throw new IllegalArgumentException("the columns must not be null and must have one length");
    int[] rows = new int[total];
    int n = 0;
    for (int i = 0; i < total; i++) {
      boolean ok = true;
      for (int j = 0; j < p && ok; j++) ok = !Double.isNaN(cols[j][i]);
      if (ok) rows[n++] = i;
    }
    if (n < 2) throw new IllegalArgumentException("PCA needs at least 2 complete rows, not " + n);
    double[] mean = new double[p];
    double[][] centred = new double[p][];
    for (int j = 0; j < p; j++) {
      double[] c = new double[n];
      for (int i = 0; i < n; i++) c[i] = cols[j][rows[i]];
      mean[j] = StatsMath.mean(c);
      for (int i = 0; i < n; i++) c[i] -= mean[j];
      centred[j] = c;
    }
    double[][] r = StatsLinear.r(centred, null, n);  // RᵀR = XᵀX of the centred columns
    double[][] cov = new double[p][p];
    for (int i = 0; i < p; i++)
      for (int j = i; j < p; j++) {
        double s = 0;
        for (int t = 0; t <= i; t++) s += r[t][i] * r[t][j];
        cov[i][j] = s / (n - 1);
      }
    return of(cov, k, mean, names, n);
  }

  /** Rows of float vectors: chunks of rows transposed and centred, their Gram matrices summed. */
  static Pca ofRows(float[][] rows, int k) {
    int n = rows.length;
    if (n < 2) throw new IllegalArgumentException("PCA needs at least 2 vectors, not " + n);
    int d = VectorEngine.dimension(rows);
    double[] mean = new double[d];
    for (float[] r : rows)
      for (int j = 0; j < d; j++) mean[j] += r[j];
    for (int j = 0; j < d; j++) mean[j] /= n;
    float[] fm = new float[d];
    for (int j = 0; j < d; j++) fm[j] = (float) mean[j];
    final int len = 512;
    float[][] t = new float[d][len];
    double[][] acc = new double[d][d];
    for (int c0 = 0; c0 < n; c0 += len) {
      int c1 = Math.min(n, c0 + len);
      for (int i = c0; i < c1; i++) {
        float[] r = rows[i];
        for (int j = 0; j < d; j++) t[j][i - c0] = r[j] - fm[j];
      }
      for (int i = c1 - c0; i < len && c1 - c0 < len; i++)  // the last chunk: zero columns add nothing
        for (int j = 0; j < d; j++) t[j][i] = 0;
      VectorEngine.gramAdd(t, len, acc);
    }
    for (int i = 0; i < d; i++)
      for (int j = i; j < d; j++) acc[i][j] /= n - 1;
    return of(acc, k, mean, null, n);
  }

  /** The components, k x p: row c is the c-th direction, unit length. */
  public double[][] components() {
    double[][] c = new double[components.length][];
    for (int i = 0; i < c.length; i++) c[i] = components[i].clone();
    return c;
  }

  /** The variance along each component (n - 1), largest first. */
  public double[] explainedVariance() { return variance.clone(); }

  /** Each component's share of the total variance. */
  public double[] explainedVarianceRatio() { return ratio.clone(); }

  /** The mean that was subtracted (one per column or dimension). */
  public double[] mean() { return mean.clone(); }

  /** The columns' names (a Frame's PCA), else null. */
  public String[] names() { return names == null ? null : names.clone(); }

  /** The rows (vectors) used. */
  public long n() { return n; }

  /**
   * Columns of numbers (p columns, the model's order) as k columns of component scores; a row with
   * a missing value gives NaN.
   */
  public double[][] transform(double[][] cols) {
    int p = mean.length;
    if (cols == null || cols.length != p)
      throw new IllegalArgumentException("the PCA has " + p + " columns, not " + (cols == null ? 0 : cols.length));
    int len = cols[0].length, k = components.length;
    double[][] out = new double[k][len];
    for (int i = 0; i < len; i++)
      for (int c = 0; c < k; c++) {
        double s = 0;
        for (int j = 0; j < p; j++) s += (cols[j][i] - mean[j]) * components[c][j];
        out[c][i] = s;
      }
    return out;
  }

  /** {@link #transform(double[][])} for JavaScript or Python columns. */
  public double[][] transform(Object cols) {
    return transform(GuestColumns.columns(cols, "the columns"));
  }

  /** A Frame's columns (by the PCA's names) as k columns of component scores. */
  public double[][] transform(Frame f) {
    if (names == null) throw new IllegalArgumentException("this PCA was made from vectors: project() them");
    double[][] cols = new double[names.length][];
    for (int j = 0; j < names.length; j++) cols[j] = f.column(names[j]);
    return transform(cols);
  }

  /** Vectors as k-dimensional vectors of component scores (a 2-D map, a smaller set to search). */
  public float[][] project(float[][] rows) {
    int p = mean.length, k = components.length;
    int d = VectorEngine.dimension(rows);
    if (rows.length > 0 && d != p) throw new IllegalArgumentException("the vectors have " + d + " dimensions, the PCA " + p);
    float[][] comp = new float[k][p];
    float[] fm = new float[p];
    for (int j = 0; j < p; j++) fm[j] = (float) mean[j];
    for (int c = 0; c < k; c++)
      for (int j = 0; j < p; j++) comp[c][j] = (float) components[c][j];
    float[][] out = new float[rows.length][k];
    int parts = VectorEngine.parts((long) rows.length * p * k, rows.length);
    VectorEngine.run(parts, parts > 1, t -> {
      float[] centred = new float[p];
      for (int i = (int) ((long) rows.length * t / parts), hi = (int) ((long) rows.length * (t + 1) / parts); i < hi; i++) {
        if ((i & 4095) == 0) VectorEngine.stopIfKilled();
        for (int j = 0; j < p; j++) centred[j] = rows[i][j] - fm[j];
        for (int c = 0; c < k; c++) out[i][c] = VectorEngine.dot(centred, comp[c]);
      }
    });
    return out;
  }

  /** {@link #project(float[][])} for JavaScript or Python vectors. */
  public float[][] project(Object rows) {
    return project(GuestVectors.matrix(rows, "the vectors"));
  }

  /** JSON: {"components": [[...]], "explainedVariance": [...], "explainedVarianceRatio": [...], "n": ...}. */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("{");
    if (names != null) {
      b.append("\"names\": [");
      for (int i = 0; i < names.length; i++) Frame.quote(b.append(i > 0 ? ", " : ""), names[i]);
      b.append("], ");
    }
    b.append("\"components\": [");
    for (int c = 0; c < components.length; c++) b.append(c > 0 ? ", " : "").append(LinearModel.array(components[c]));
    return b.append("], \"explainedVariance\": ").append(LinearModel.array(variance))
        .append(", \"explainedVarianceRatio\": ").append(LinearModel.array(ratio)).append(", \"n\": ").append(n)
        .append('}').toString();
  }
}
