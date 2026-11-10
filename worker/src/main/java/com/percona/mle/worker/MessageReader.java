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

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;

/**
 * Reads a message body in place (from the arena or the reassembly buffer). The host is trusted,
 * but a malformed message still ends in an exception, never in a read outside the body.
 */
final class MessageReader {
  private final MemorySegment s;
  private long pos;

  MessageReader(MemorySegment body) {
    this.s = body;
  }

  private void need(long n) {
    if (n < 0 || pos + n > s.byteSize()) throw new IllegalStateException("message truncated");
  }

  long position() {
    return pos;
  }

  boolean atEnd() {
    return pos == s.byteSize();
  }

  void finish() {
    if (!atEnd()) throw new IllegalStateException("trailing bytes in message");
  }

  int u8() {
    need(1);
    return Byte.toUnsignedInt(s.get(ValueLayout.JAVA_BYTE, pos++));
  }

  boolean flag() {
    return u8() != 0;
  }

  int u16() {
    need(2);
    int v = Short.toUnsignedInt(s.get(ValueLayout.JAVA_SHORT_UNALIGNED, pos));
    pos += 2;
    return v;
  }

  /** As a non-negative long. */
  long u32() {
    need(4);
    long v = Integer.toUnsignedLong(s.get(ValueLayout.JAVA_INT_UNALIGNED, pos));
    pos += 4;
    return v;
  }

  int count() {
    long n = u32();
    if (n > s.byteSize() - pos) throw new IllegalStateException("count beyond message");
    return (int) n;
  }

  long u64() {
    need(8);
    long v = s.get(ValueLayout.JAVA_LONG_UNALIGNED, pos);
    pos += 8;
    return v;
  }

  float f32() {
    need(4);
    float v = s.get(ValueLayout.JAVA_FLOAT_UNALIGNED, pos);
    pos += 4;
    return v;
  }

  double f64() {
    need(8);
    double v = s.get(ValueLayout.JAVA_DOUBLE_UNALIGNED, pos);
    pos += 8;
    return v;
  }

  byte[] bytes() {
    long n = u32();
    need(n);
    byte[] b = s.asSlice(pos, n).toArray(ValueLayout.JAVA_BYTE);
    pos += n;
    return b;
  }

  String str() {
    return new String(bytes(), StandardCharsets.UTF_8);
  }

  void skipAlignment(int alignment) {
    long pad = (alignment - pos % alignment) % alignment;
    need(pad);
    pos += pad;
  }

  /** A slice of the body (no copy). */
  MemorySegment slice(long n) {
    need(n);
    MemorySegment r = s.asSlice(pos, n);
    pos += n;
    return r;
  }
}
