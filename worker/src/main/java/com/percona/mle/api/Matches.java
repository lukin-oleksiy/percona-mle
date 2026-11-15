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
 * The best rows for one query ({@link Vectors}, {@link VectorSet}), best first: the rows'
 * positions, their scores and, for a {@link VectorSet}, their ids. Scores: the cosine similarity
 * or the dot product (highest first), or the Euclidean distance (lowest first). Fewer than k when
 * there are fewer rows. From JavaScript and Python: {@code m.size()}, {@code m.id(i)}, {@code
 * m.score(i)}, or {@code String(m)} / {@code str(m)} for JSON.
 */
public final class Matches {
  private final int[] index;
  private final float[] score;
  private final long[] ids;

  Matches(int[] index, float[] score, long[] ids) {
    this.index = index;
    this.score = score;
    this.ids = ids;
  }

  /** How many rows matched (at most k). */
  public int size() {
    return index.length;
  }

  /** The i-th best row's position in the rows (or in the VectorSet). */
  public int index(int i) {
    return index[i];
  }

  /** The i-th best row's score. */
  public float score(int i) {
    return score[i];
  }

  /** The i-th best row's id (VectorSet), else its position. */
  public long id(int i) {
    return ids != null ? ids[i] : index[i];
  }

  /** All positions, best first (a copy). */
  public int[] indexes() {
    return index.clone();
  }

  /** All scores, best first (a copy). */
  public float[] scores() {
    return score.clone();
  }

  /** All ids, best first (a copy; positions when there are no ids). */
  public long[] ids() {
    long[] r = new long[index.length];
    for (int i = 0; i < r.length; i++) r[i] = id(i);
    return r;
  }

  /** JSON: [{"id": 7, "score": 0.93}, ...] (ids are positions when there are none). */
  @Override
  public String toString() {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < index.length; i++) {
      if (i > 0) b.append(", ");
      b.append("{\"id\": ").append(id(i)).append(", \"score\": ");
      float s = score[i];
      if (Float.isFinite(s)) b.append(s);
      else b.append("null");
      b.append('}');
    }
    return b.append(']').toString();
  }
}
