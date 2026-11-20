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
 * Reads a message body of the host in place (from the arena or the reassembly buffer). A body that
 * is not what §7 and §9 define is a protocol failure: the process ends (ProtocolFailure), nothing
 * is thrown.
 */
final class MessageReader {
  private final MemorySegment s;
  private long pos;

  MessageReader(MemorySegment body) {
    this.s = body;
  }

  private void need(long n) {
    if (n < 0 || n > s.byteSize() - pos) throw ProtocolFailure.exit("message body truncated");
  }

  long position() {
    return pos;
  }

  long remaining() {
    return s.byteSize() - pos;
  }

  /** The body is consumed exactly (§5.1). */
  void finish() {
    if (pos != s.byteSize()) throw ProtocolFailure.exit("trailing bytes in a message body");
  }

  int u8() {
    need(1);
    return Byte.toUnsignedInt(s.get(ValueLayout.JAVA_BYTE, pos++));
  }

  boolean bool() {
    int v = u8();
    if (v > 1) throw ProtocolFailure.exit("a bool of " + v);
    return v == 1;
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
   * bytes left and against the list's limit before anything is allocated for it (§7.1).
   */
  int count(long minSize, long limit) {
    long n = u32();
    if (n > limit || n > remaining() / minSize) throw ProtocolFailure.exit("a list of " + n + " elements");
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

  /** A str: well-formed UTF-8, or the host broke the protocol. */
  String str() {
    String t = Text.decode(bytes());
    if (t == null) throw ProtocolFailure.exit("a str that is not well-formed UTF-8");
    return t;
  }

  /** An lstr: the server's data, which may be ill-formed UTF-8 and is repaired here (§7.1). */
  String lstr() {
    return Text.decodeRepairing(bytes());
  }

  /** The zero bytes up to the next multiple of alignment, counted from the start of the body. */
  void skipPadding(int alignment) {
    long pad = (alignment - pos % alignment) % alignment;
    need(pad);
    for (long i = 0; i < pad; i++)
      if (s.get(ValueLayout.JAVA_BYTE, pos + i) != 0) throw ProtocolFailure.exit("VECTOR padding is not zero");
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
