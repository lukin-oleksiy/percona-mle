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
import java.util.concurrent.locks.LockSupport;

/**
 * The worker's side of the shared memory mailbox (§3 to §5): mapped with FFM, sequence words read
 * and written with VarHandle atomics, futex waits through a syscall downcall.
 *
 * <p>Messages are decoded in place from the arena when they fit in one chunk; larger messages
 * are reassembled in a native buffer. Messages are written in place by {@link MessageWriter}.
 * Every header of the host is checked in full; one that breaks §5 is a protocol failure.
 */
final class Mailbox {
  private static final VarHandle INT = ValueLayout.JAVA_INT.varHandle();
  /** The bytes kept of a message that is discarded (PREPARE's routine_id is in them). */
  private static final long HEAD = 8;

  final MemorySegment region;
  final MemorySegment arena;
  final int arenaSize;
  /** The region's version (§3). */
  final int version;
  private final MemorySegment toW;
  private final MemorySegment toH;
  private final int spin;
  /** The value of the host's word seen last (§4.1). */
  private int seenW;
  /** The worker's post count: stored into its word, never read back from it. */
  private int posts;
  private MemorySegment assembly = MemorySegment.NULL;
  /** The last message was a large one: its buffer goes when the next message arrives. */
  private boolean dropAssembly;

  /**
   * A received message. body is valid until the worker posts its next message. discarded: the
   * worker could not hold the message (§5.3); the transfer was completed, and body is only its
   * first bytes (at most 8).
   */
  record Message(int op, int depth, long callSeq, MemorySegment body, boolean discarded, boolean chunked) {}

  /**
   * What the receiver expects (§8): called with the first chunk's header before anything is
   * answered; it ends the process (ProtocolFailure) for a message that is out of place.
   */
  interface Admission {
    void check(int op, int depth, long callSeq);
  }

  private Mailbox(MemorySegment region, int arenaSize, int version, int spin) {
    this.region = region;
    this.arenaSize = arenaSize;
    this.version = version;
    this.arena = region.asSlice(Wire.HEADER_SIZE, arenaSize);
    this.toW = region.asSlice(Wire.OFF_TO_W, 4);
    this.toH = region.asSlice(Wire.OFF_TO_H, 4);
    this.spin = spin;
  }

  /**
   * Maps the region (§2 step 3). A wrong magic or size ends the process with status 2 before the
   * region is touched.
   */
  static Mailbox open(int spin) {
    long fileSize = Native.fileSize(Wire.MAILBOX_FD);
    if (fileSize < Wire.HEADER_SIZE) Runtime.getRuntime().halt(Wire.EXIT_BAD_REGION);
    MemorySegment header = Native.mapShared(Wire.MAILBOX_FD, Wire.HEADER_SIZE);
    int magic = header.get(ValueLayout.JAVA_INT, Wire.OFF_MAGIC);
    int version = header.get(ValueLayout.JAVA_INT, Wire.OFF_VERSION);
    long size = header.get(ValueLayout.JAVA_LONG, Wire.OFF_REGION_SIZE);
    long arenaSize = Integer.toUnsignedLong(header.get(ValueLayout.JAVA_INT, Wire.OFF_ARENA_SIZE));
    Native.unmap(header);
    if (magic != Wire.MAGIC || size != Wire.HEADER_SIZE + arenaSize || size != fileSize)
      Runtime.getRuntime().halt(Wire.EXIT_BAD_REGION);
    // The launcher keeps §3's bounds of the arena; the one-chunk messages rely on the lower one.
    if (arenaSize < Wire.MIN_ARENA_SIZE || arenaSize > Wire.MAX_ARENA_SIZE || arenaSize % 4096 != 0)
      throw new IllegalStateException("an arena of " + arenaSize + " bytes");
    return new Mailbox(Native.mapShared(Wire.MAILBOX_FD, size), (int) arenaSize, version, spin);
  }

  // ------------------------------------------------------------ raw hand-off

  /** Waits until the host's word differs from the value seen: spin, then sleep on the futex (§4.3). */
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
      INT.setRelease(region, Wire.OFF_W_SLEEP, 0);
      v = (int) INT.getAcquire(region, Wire.OFF_TO_W);
      if (v != seenW) {
        seenW = v;
        return;
      }
    }
  }

  /** True if the host has sent a message that receive() has not taken yet. */
  boolean pending() {
    return (int) INT.getAcquire(region, Wire.OFF_TO_W) != seenW;
  }

  /** Waits up to {@code nanos} for a message from the host (polling every 1 ms); true if one came. */
  boolean awaitPending(long nanos) {
    long end = System.nanoTime() + nanos;
    while (!pending()) {
      if (System.nanoTime() - end >= 0) return false;
      LockSupport.parkNanos(1_000_000L);
    }
    return true;
  }

  /**
   * Posts what is in msg and the arena (§4.1, §4.2): the new count is stored into the worker's
   * word, and that store and the load of the host's sleep flag are sequentially consistent.
   */
  private void post() {
    INT.setVolatile(region, Wire.OFF_TO_H, ++posts);
    if ((int) INT.getVolatile(region, Wire.OFF_H_SLEEP) != 0) Native.futexWake(toH);
  }

  private void header(int op, int status, int flags, int depth, long len, long total, long seq) {
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_OP, op);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_STATUS, status);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_FLAGS, flags);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_DEPTH, depth);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_LEN, (int) len);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_MSG + Wire.MSG_RESERVED, 0);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_MSG + Wire.MSG_TOTAL_LEN, total);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_MSG + Wire.MSG_CALL_SEQ, seq);
  }

  private int msgInt(long field) {
    return region.get(ValueLayout.JAVA_INT, Wire.OFF_MSG + field);
  }

  private long msgLong(long field) {
    return region.get(ValueLayout.JAVA_LONG, Wire.OFF_MSG + field);
  }

  private static boolean hostOp(int op) {
    return switch (op) {
      case Wire.PREPARE, Wire.CALL, Wire.SQL_RESULT, Wire.DROP, Wire.RESET, Wire.OPTIONS, Wire.PARSE_LIBRARY,
           Wire.LIBRARY_RESULT -> true;
      default -> false;
    };
  }

  // ------------------------------------------------------------ messages

  /**
   * Waits for the next message of the host and reassembles it if it comes in chunks (§5.2). The
   * first chunk's header is checked as a whole message's would be, admission included, before
   * the first CONTINUE; every later chunk must repeat it.
   */
  Message receive(Admission admission) {
    waitHost();
    int op = msgInt(Wire.MSG_OP);
    int status = msgInt(Wire.MSG_STATUS);
    int flags = msgInt(Wire.MSG_FLAGS);
    int depth = msgInt(Wire.MSG_DEPTH);
    long len = Integer.toUnsignedLong(msgInt(Wire.MSG_LEN));
    int reserved = msgInt(Wire.MSG_RESERVED);
    long total = msgLong(Wire.MSG_TOTAL_LEN);
    long seq = msgLong(Wire.MSG_CALL_SEQ);
    if (dropAssembly) {  // a large message's buffer is not kept for the worker's life
      assembly = MemorySegment.NULL;
      dropAssembly = false;
    }
    if (!hostOp(op)) throw ProtocolFailure.exit("op " + op + " from the host");
    if (status != 0 || reserved != 0 || (flags & ~Wire.FLAG_MORE) != 0)
      throw ProtocolFailure.exit("bad header of op " + op + ": status " + status + ", flags " + flags);
    boolean more = flags == Wire.FLAG_MORE;
    boolean small = op == Wire.OPTIONS || op == Wire.DROP || op == Wire.RESET;
    if (total < 0 || total > (small ? Wire.MAX_SMALL_MESSAGE : Wire.MAX_HOST_MESSAGE))
      throw ProtocolFailure.exit("op " + op + " of " + Long.toUnsignedString(total) + " bytes");
    // One chunk is the whole message; of several, every one but the last is a whole arena.
    if (more ? total <= arenaSize || len != arenaSize : len != total || total > arenaSize)
      throw ProtocolFailure.exit("bad chunk of op " + op + ": len " + len + " of " + total);
    admission.check(op, depth, seq);
    if (!more) return new Message(op, depth, seq, arena.asSlice(0, len), false, false);

    // A message the worker has no memory for is still received to its end, without its bytes (§5.3).
    boolean discard = false;
    MemorySegment head = MemorySegment.NULL;
    if (assembly.byteSize() < total) {
      try {
        assembly = Arena.ofAuto().allocate(total, Wire.VECTOR_ALIGNMENT);
      } catch (OutOfMemoryError e) {
        discard = true;
        head = MemorySegment.ofArray(arena.asSlice(0, HEAD).toArray(ValueLayout.JAVA_BYTE));
      }
    }
    dropAssembly = assembly.byteSize() > 4L * arenaSize;
    long have = 0;
    while (true) {
      if (!discard) MemorySegment.copy(arena, 0, assembly, have, len);
      have += len;
      if (!more) break;
      header(Wire.CONTINUE, 0, 0, depth, 0, 0, seq);
      post();
      waitHost();
      flags = msgInt(Wire.MSG_FLAGS);
      len = Integer.toUnsignedLong(msgInt(Wire.MSG_LEN));
      if (msgInt(Wire.MSG_OP) != op || msgInt(Wire.MSG_STATUS) != 0 || msgInt(Wire.MSG_DEPTH) != depth
          || msgInt(Wire.MSG_RESERVED) != 0 || msgLong(Wire.MSG_TOTAL_LEN) != total
          || msgLong(Wire.MSG_CALL_SEQ) != seq || (flags & ~Wire.FLAG_MORE) != 0)
        throw ProtocolFailure.exit("a chunk of op " + op + " that does not repeat the first chunk's header");
      more = flags == Wire.FLAG_MORE;
      if (more ? len != arenaSize || have + len >= total : len < 1 || len > arenaSize || have + len != total)
        throw ProtocolFailure.exit("bad chunk of op " + op + ": len " + len + " after " + have + " of " + total);
    }
    return new Message(op, depth, seq, discard ? head : assembly.asSlice(0, total), discard, true);
  }

  /**
   * Posts the message in w (written in place, or spilled and sent in chunks). The writer keeps
   * the size limits of §12; between the chunks the host posts CONTINUE and nothing else.
   */
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
      if (msgInt(Wire.MSG_OP) != Wire.CONTINUE || msgInt(Wire.MSG_STATUS) != 0 || msgInt(Wire.MSG_FLAGS) != 0
          || msgInt(Wire.MSG_DEPTH) != depth || msgInt(Wire.MSG_LEN) != 0 || msgInt(Wire.MSG_RESERVED) != 0
          || msgLong(Wire.MSG_TOTAL_LEN) != 0 || msgLong(Wire.MSG_CALL_SEQ) != seq)
        throw ProtocolFailure.exit("expected CONTINUE for op " + op);
    }
  }

  // ------------------------------------------------------------ stats

  void stats(int state, boolean tainted) {
    Runtime rt = Runtime.getRuntime();
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_USED, rt.totalMemory() - rt.freeMemory());
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_MAX, rt.maxMemory());
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_GC_COUNT, MemoryWatch.gcs());
    region.set(ValueLayout.JAVA_INT, Wire.OFF_STATS + Wire.STATS_STATE, state);
    region.set(ValueLayout.JAVA_INT, Wire.OFF_STATS + Wire.STATS_TAINTED, tainted ? 1 : 0);
  }
}
