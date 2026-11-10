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
import java.lang.invoke.VarHandle;

/**
 * The worker's side of the shared memory mailbox (protocol.h): mapped with FFM, sequence words
 * read and written with VarHandle atomics, futex waits through a syscall downcall.
 *
 * <p>Messages are decoded in place from the arena when they fit in one chunk; larger messages
 * are reassembled in a native buffer. Messages are written in place by {@link MessageWriter}.
 */
final class Mailbox {
  private static final VarHandle INT = ValueLayout.JAVA_INT.varHandle();

  final MemorySegment region;
  final MemorySegment arena;
  final int arenaSize;
  private final MemorySegment toW;
  private final MemorySegment toH;
  private final int spin;
  private int seenW;
  private MemorySegment assembly = MemorySegment.NULL;

  /** A received message. body is valid until the worker posts its next message. */
  record Message(int op, int status, int depth, long callSeq, MemorySegment body) {}

  private Mailbox(MemorySegment region, int spin) {
    this.region = region;
    this.arenaSize = region.get(ValueLayout.JAVA_INT, Wire.OFF_ARENA_SIZE);
    this.arena = region.asSlice(Wire.HEADER_SIZE, arenaSize);
    this.toW = region.asSlice(Wire.OFF_TO_W, 4);
    this.toH = region.asSlice(Wire.OFF_TO_H, 4);
    this.spin = spin;
  }

  static Mailbox open(int spin) {
    MemorySegment header = Native.mapShared(Wire.MAILBOX_FD, Wire.HEADER_SIZE);
    int magic = header.get(ValueLayout.JAVA_INT, Wire.OFF_MAGIC);
    int version = header.get(ValueLayout.JAVA_INT, Wire.OFF_VERSION);
    long size = header.get(ValueLayout.JAVA_LONG, Wire.OFF_REGION_SIZE);
    Native.unmap(header);
    if (magic != Wire.MAGIC || version != Wire.VERSION)
      throw new IllegalStateException("not an MLE v1 mailbox");
    return new Mailbox(Native.mapShared(Wire.MAILBOX_FD, size), spin);
  }

  // ------------------------------------------------------------ raw hand-off

  /** Waits until the host bumps to_w: spin, then sleep on the futex. */
  private void waitHost() {
    for (int i = 0; i < spin; i++) {
      int v = (int) INT.getAcquire(region, Wire.OFF_TO_W);
      if (v != seenW) {
        seenW = v;
        return;
      }
      Thread.onSpinWait();
    }
    while (true) {
      INT.setVolatile(region, Wire.OFF_W_SLEEP, 1);
      int v = (int) INT.getVolatile(region, Wire.OFF_TO_W);
      if (v == seenW) Native.futexWait(toW, seenW);
      INT.setVolatile(region, Wire.OFF_W_SLEEP, 0);
      v = (int) INT.getAcquire(region, Wire.OFF_TO_W);
      if (v != seenW) {
        seenW = v;
        return;
      }
    }
  }

  private void post() {
    int unused = (int) INT.getAndAdd(region, Wire.OFF_TO_H, 1);
    if ((int) INT.getVolatile(region, Wire.OFF_H_SLEEP) != 0) Native.futexWake(toH);
  }

  private void header(int op, int status, int flags, int depth, long len, long total, long seq) {
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_OP, op);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_STATUS, status);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_FLAGS, flags);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_DEPTH, depth);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_LEN, (int) len);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_MSG + Wire.MSG_TOTAL_LEN, total);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_MSG + Wire.MSG_CALL_SEQ, seq);
  }

  private int msgInt(long field) {
    return region.get(ValueLayout.JAVA_INT, Wire.OFF_MSG + field);
  }

  private long msgLong(long field) {
    return region.get(ValueLayout.JAVA_LONG, Wire.OFF_MSG + field);
  }

  // ------------------------------------------------------------ messages

  /** Waits for the next message from the host and reassembles it if it is chunked. */
  Message receive() {
    waitHost();
    int op = msgInt(Wire.MSG_OP);
    int status = msgInt(Wire.MSG_STATUS);
    int flags = msgInt(Wire.MSG_FLAGS);
    int depth = msgInt(Wire.MSG_DEPTH);
    long len = Integer.toUnsignedLong(msgInt(Wire.MSG_LEN));
    long total = msgLong(Wire.MSG_TOTAL_LEN);
    long seq = msgLong(Wire.MSG_CALL_SEQ);
    if (len > arenaSize || len > total) throw new IllegalStateException("bad message header");
    if ((flags & Wire.FLAG_MORE) == 0) return new Message(op, status, depth, seq, arena.asSlice(0, len));

    if (assembly.byteSize() < total) assembly = Arena.ofAuto().allocate(total, Wire.VECTOR_ALIGNMENT);
    long have = 0;
    while (true) {
      MemorySegment.copy(arena, 0, assembly, have, len);
      have += len;
      if ((flags & Wire.FLAG_MORE) == 0) break;
      header(Wire.CONTINUE, Wire.OK, 0, depth, 0, 0, seq);
      post();
      waitHost();
      flags = msgInt(Wire.MSG_FLAGS);
      len = Integer.toUnsignedLong(msgInt(Wire.MSG_LEN));
      if (msgInt(Wire.MSG_OP) != op || msgLong(Wire.MSG_CALL_SEQ) != seq || len > arenaSize || have + len > total)
        throw new IllegalStateException("bad message chunk");
    }
    if (have != total) throw new IllegalStateException("truncated message");
    return new Message(op, status, depth, seq, assembly.asSlice(0, total));
  }

  /** Posts the message in w (written in place, or spilled and sent in chunks). */
  void send(int op, int status, int depth, long seq, MessageWriter w) {
    long total = w.size();
    if (!w.spilled()) {
      header(op, status, 0, depth, total, total, seq);
      post();
      return;
    }
    MemorySegment body = w.body();
    long sent = 0;
    while (true) {
      long len = Math.min(arenaSize, total - sent);
      MemorySegment.copy(body, sent, arena, 0, len);
      boolean more = sent + len < total;
      header(op, status, more ? Wire.FLAG_MORE : 0, depth, len, total, seq);
      post();
      sent += len;
      if (!more) return;
      waitHost();
      if (msgInt(Wire.MSG_OP) != Wire.CONTINUE) throw new IllegalStateException("expected CONTINUE");
    }
  }

  // ------------------------------------------------------------ stats

  void stats(int state, boolean tainted) {
    Runtime rt = Runtime.getRuntime();
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_USED, rt.totalMemory() - rt.freeMemory());
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_MAX, rt.maxMemory());
    region.set(ValueLayout.JAVA_INT, Wire.OFF_STATS + Wire.STATS_STATE, state);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_STATS + Wire.STATS_TAINTED, tainted ? 1 : 0);
  }
}
