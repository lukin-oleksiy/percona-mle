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

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;

/**
 * Encodes a message body directly into the mailbox arena. When the message outgrows the arena it
 * "spills" into a growing native buffer, and {@link Mailbox#send} sends it in chunks.
 */
final class MessageWriter {
  /**
   * The message would be larger than the host accepts (Wire.MAX_WORKER_MESSAGE): thrown before
   * anything larger is allocated, so an oversized value cannot exhaust the worker's memory here.
   */
  static final class TooLarge extends RuntimeException {
    @java.io.Serial private static final long serialVersionUID = 1L;

    TooLarge() {
      super(null, null, false, false);
    }
  }

  private final MemorySegment arena;
  private MemorySegment buf;
  private long pos;
  private boolean spilled;

  MessageWriter(MemorySegment arena) {
    this.arena = arena;
    reset();
  }

  void reset() {
    buf = arena;
    pos = 0;
    spilled = false;
  }

  long size() {
    return pos;
  }

  boolean spilled() {
    return spilled;
  }

  MemorySegment body() {
    return buf.asSlice(0, pos);
  }

  long position() {
    return pos;
  }

  private void ensure(long n) {
    if (pos + n <= buf.byteSize()) return;
    if (n < 0 || pos + n > Wire.MAX_WORKER_MESSAGE) throw new TooLarge();
    long size = Math.min(Math.max(buf.byteSize() * 2, pos + n), Wire.MAX_WORKER_MESSAGE);
    MemorySegment bigger = Arena.ofAuto().allocate(size, Wire.VECTOR_ALIGNMENT);
    MemorySegment.copy(buf, 0, bigger, 0, pos);
    buf = bigger;
    spilled = true;
  }

  void u8(int v) {
    ensure(1);
    buf.set(ValueLayout.JAVA_BYTE, pos, (byte) v);
    pos += 1;
  }

  void u16(int v) {
    ensure(2);
    buf.set(ValueLayout.JAVA_SHORT_UNALIGNED, pos, (short) v);
    pos += 2;
  }

  void u32(long v) {
    ensure(4);
    buf.set(ValueLayout.JAVA_INT_UNALIGNED, pos, (int) v);
    pos += 4;
  }

  void u64(long v) {
    ensure(8);
    buf.set(ValueLayout.JAVA_LONG_UNALIGNED, pos, v);
    pos += 8;
  }

  void f32(float v) {
    ensure(4);
    buf.set(ValueLayout.JAVA_FLOAT_UNALIGNED, pos, v);
    pos += 4;
  }

  void f64(double v) {
    ensure(8);
    buf.set(ValueLayout.JAVA_DOUBLE_UNALIGNED, pos, v);
    pos += 8;
  }

  void bytes(byte[] b) {
    u32(b.length);
    ensure(b.length);
    MemorySegment.copy(b, 0, buf, ValueLayout.JAVA_BYTE, pos, b.length);
    pos += b.length;
  }

  void str(String s) {
    bytes(s.getBytes(StandardCharsets.UTF_8));
  }

  /** Zero bytes up to the next multiple of alignment (relative to the message start). */
  void align(int alignment) {
    long pad = (alignment - pos % alignment) % alignment;
    ensure(pad);
    buf.asSlice(pos, pad).fill((byte) 0);
    pos += pad;
  }

  void floats(MemorySegment src, long count) {
    ensure(count * 4);
    MemorySegment.copy(src, 0, buf, pos, count * 4);
    pos += count * 4;
  }

  void floats(float[] f) {
    ensure(f.length * 4L);
    MemorySegment.copy(f, 0, buf, ValueLayout.JAVA_FLOAT_UNALIGNED, pos, f.length);
    pos += f.length * 4L;
  }
}
