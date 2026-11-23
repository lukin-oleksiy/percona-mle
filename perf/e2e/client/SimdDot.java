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

package e2e;

import jdk.incubator.vector.FloatVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

/** The external Java application's SIMD dot product (the JDK's Vector API), as MLE's vector helper has. */
public final class SimdDot {
  private SimdDot() {}

  private static final VectorSpecies<Float> S = FloatVector.SPECIES_PREFERRED;

  public static float dot(float[] a, float[] b) {
    FloatVector acc = FloatVector.zero(S);
    int i = 0, upper = S.loopBound(a.length);
    for (; i < upper; i += S.length()) acc = FloatVector.fromArray(S, a, i).fma(FloatVector.fromArray(S, b, i), acc);
    float sum = acc.reduceLanes(VectorOperators.ADD);
    for (; i < a.length; i++) sum += a[i] * b[i];
    return sum;
  }
}
