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
 * Pairs of similar rows ({@link VectorSet#pairs}, {@link Vectors#pairs}), best first: the two rows'
 * positions (first < second), their ids and the pair's score (a similarity, or a distance when
 * {@link #distance()}). Ties: the lower first row, then the lower second row. JSON with {@code
 * String(p)} / {@code str(p)}.
 */
public final class Pairs {
  private final int[] first, second;
  private final float[] score;
  private final long[] ids;
  private final boolean distance;

  Pairs(int[] first, int[] second, float[] score, long[] ids, boolean distance) {
    this.first = first;
    this.second = second;
    this.score = score;
    this.ids = ids;
    this.distance = distance;
  }

  /** How many pairs. */
  public int size() {
    return first.length;
  }

  /** The i-th pair's first row (position). */
  public int first(int i) {
    return first[i];
  }

  /** The i-th pair's second row (position, greater than the first). */
  public int second(int i) {
    return second[i];
  }

  /** The id of the i-th pair's first row (its position when there are no ids). */
  public long firstId(int i) {
    return ids != null ? ids[first[i]] : first[i];
  }

  /** The id of the i-th pair's second row. */
  public long secondId(int i) {
    return ids != null ? ids[second[i]] : second[i];
  }

  /** The i-th pair's score. */
  public float score(int i) {
    return score[i];
  }

  /** Whether the scores are distances (lower is better). */
  public boolean distance() {
    return distance;
  }

  /** JSON: [{"first": 3, "second": 9, "score": 0.98}, ...] (ids). */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < first.length; i++) {
      if (i > 0) b.append(", ");
      b.append("{\"first\": ").append(firstId(i)).append(", \"second\": ").append(secondId(i))
          .append(", \"score\": ").append(score[i]).append('}');
    }
    return b.append(']').toString();
  }
}
