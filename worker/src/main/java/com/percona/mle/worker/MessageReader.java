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

/**
 * Reads a message body of the host in place (from the arena or the reassembly buffer). The host
 * is trusted (§1): the reader checks every length and count against the bytes of the body, so
 * that nothing outside the message is read, and nothing else. A length or count beyond the body
 * is a protocol failure: the process ends (ProtocolFailure), nothing is thrown. Text is decoded
 * as it comes (ill-formed UTF-8 becomes U+FFFD), a bool is true for any byte but 0, padding is
 * skipped unread, and bytes after the last field are left alone.
 */
final class MessageReader {
  private MemorySegment s;
  private long pos;
  private long end;

  MessageReader() {
    this(MemorySegment.NULL);
  }

  MessageReader(MemorySegment body) {
    reset(body, body.byteSize());
  }

  /** Reads the first length bytes of body from now on. */
  MessageReader reset(MemorySegment body, long length) {
    s = body;
    pos = 0;
    end = Math.min(length, body.byteSize());
    return this;
  }

  private void need(long n) {
    if (n < 0 || n > end - pos) throw ProtocolFailure.exit("message body truncated");
  }

  long position() {
    return pos;
  }

  long remaining() {
    return end - pos;
  }

  int u8() {
    need(1);
    return Byte.toUnsignedInt(s.get(ValueLayout.JAVA_BYTE, pos++));
  }

  boolean bool() {
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

  /**
   * The count of a list whose elements take at least minSize bytes each: checked against the
   * bytes left before anything is allocated for it (§1, §7.1).
   */
  int count(long minSize) {
    long n = u32();
    if (n > remaining() / minSize) throw ProtocolFailure.exit("a list of " + n + " elements");
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
    byte[] b = new byte[(int) n];
    MemorySegment.copy(s, ValueLayout.JAVA_BYTE, pos, b, 0, (int) n);
    pos += n;
    return b;
  }

  /** A str: the host sends well-formed UTF-8, and the worker does not check it (§1). */
  String str() {
    return Text.decodeRepairing(bytes());
  }

  /** An lstr: the server's data, which may be ill-formed UTF-8 and is repaired here (§7.1). */
  String lstr() {
    return Text.decodeRepairing(bytes());
  }

  /** The padding up to the next multiple of alignment, counted from the start of the body. */
  void skipPadding(int alignment) {
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
