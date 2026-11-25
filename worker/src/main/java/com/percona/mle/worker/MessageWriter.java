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

/**
 * Encodes a message body directly into the mailbox arena. When the message outgrows the arena it
 * "spills" into a growing native buffer, and {@link Mailbox#send} sends it in chunks. The worker
 * has the turn while it composes: nothing that could post a message (guest code, a nested request)
 * runs between reset() and the post (§4.1).
 */
final class MessageWriter {
  /**
   * The message would be larger than its limit (§12): thrown before anything larger is
   * allocated, so an oversized value cannot exhaust the worker's memory here.
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
  private long limit;
  private boolean spilled;

  MessageWriter(MemorySegment arena) {
    this.arena = arena;
    reset();
  }

  /** A new message of at most 64 MiB. */
  void reset() {
    reset(Wire.MAX_WORKER_MESSAGE);
  }

  /** A new message of at most limit bytes (64 KiB for the messages of one chunk). */
  void reset(long limit) {
    buf = arena;
    pos = 0;
    this.limit = limit;
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
    if (n < 0 || pos + n > limit) throw new TooLarge();
    if (pos + n <= buf.byteSize()) return;
    long size = Math.min(Math.max(buf.byteSize() * 2, pos + n), limit);
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

  void bool(boolean v) {
    u8(v ? 1 : 0);
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
    bytes(b, 0, b.length);
  }

  void bytes(byte[] b, int from, int length) {
    u32(length);
    ensure(length);
    MemorySegment.copy(b, from, buf, ValueLayout.JAVA_BYTE, pos, length);
    pos += length;
  }

  /**
   * A str or lstr: well-formed UTF-8. The text is data or a message of guest code: an unpaired
   * surrogate becomes U+FFFD (§7.1). Code and names are checked by their senders (Text.wellFormed)
   * before they get here, so for them this changes nothing.
   */
  void str(String s) {
    // UTF-8 has at least one byte per UTF-16 unit: text longer than the room left is refused
    // before it is encoded (a huge string would otherwise exhaust the heap there; MVP review JVMP-1).
    if (s.length() > limit - pos) throw new TooLarge();
    bytes(Text.utf8(s));
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
