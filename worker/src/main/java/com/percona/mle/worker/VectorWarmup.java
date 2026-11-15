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

import com.percona.mle.api.Vectors;
import java.util.Arrays;
import java.util.Random;
import java.util.function.BooleanSupplier;

/**
 * The vector helper's warm-up (mle.pool_prewarm=vectors, exp note 35): runs its kernels on random
 * data until the JIT has compiled them, so a worker's first real call runs at full speed.
 *
 * <p>Measured in a fresh JVM with the worker's options: about 0.55 s and 2.3 CPU-seconds (mostly
 * the JIT); afterwards the first query (10k x 768) takes 3 ms instead of ~110 ms and the first
 * {@code related} 0.21 s instead of ~0.5 s (4 threads). The small steps come first, while the code
 * is still interpreted; the larger {@code related} steps then warm what only bigger sets reach
 * (the shared top-k lists); without them the first large call was warm in only half of the runs.
 * No step takes more than ~70 ms, so a request arriving meanwhile waits at most that long.
 */
final class VectorWarmup {
  private VectorWarmup() {}

  private static final int DIMENSIONS = 768;
  private static final int ROWS = 512;
  private static final int FEW = 128;
  private static final int SMALL_ROUNDS = 20;
  private static final int LARGE_ROUNDS = 8;

  /**
   * Runs the warm-up, checking {@code stop} between steps.
   *
   * @return true if it ran to the end, false if {@code stop} ended it
   */
  static boolean run(BooleanSupplier stop) {
    Random random = new Random(1);
    float[][] rows = new float[ROWS][DIMENSIONS];
    for (float[] row : rows)
      for (int i = 0; i < row.length; i++) row[i] = (float) random.nextGaussian();
    float[][] few = Arrays.copyOf(rows, FEW);
    for (int i = 0; i < SMALL_ROUNDS; i++) {
      for (Vectors.Metric m : Vectors.Metric.values()) {
        if (stop.getAsBoolean()) return false;
        Vectors.nearest(rows[i], rows, 10, m);
        if (stop.getAsBoolean()) return false;
        Vectors.related(few, 10, m);
      }
      Vectors.cosine(rows[0], rows[i]);
    }
    for (int i = 0; i < LARGE_ROUNDS; i++)
      for (Vectors.Metric m : Vectors.Metric.values()) {
        if (stop.getAsBoolean()) return false;
        Vectors.related(rows, 10, m);
      }
    return true;
  }
}
