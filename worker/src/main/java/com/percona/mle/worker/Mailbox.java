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
 * <p>Messages are decoded in place when they fit in one chunk (from the host line if they have at
 * most 40 bytes, §5.1, else from the arena); larger messages are reassembled in a native buffer. Messages are written in place by {@link MessageWriter}.
 *
 * <p>The host is trusted (§1): of its headers the worker checks what its own safety needs, which
 * is the operation, that its state can take the message (the receiver's Admission: op, depth,
 * call_seq), and every chunk's len against the arena and the bytes still due. A header that
 * fails one of these is a protocol failure; status, reserved fields, unknown flag bits and the
 * exact fields of a CONTINUE are not looked at.
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
  /** The inline bodies (§5.1): the host's, read, and the worker's, written. */
  private final MemorySegment hostBody;
  private final MemorySegment workerBody;
  private final int spin;
  /** The value of the host's word seen last (§4.1). */
  private int seenW;
  /** The worker's post count: stored into its word, never read back from it. */
  private int posts;
  private MemorySegment assembly = MemorySegment.NULL;
  /** The last message was a large one: its buffer goes when the next message arrives. */
  private boolean dropAssembly;

  /**
   * The message received last: valid until the next receive (one object, reused). length bytes
   * of body from offset 0, valid until the worker posts its next message. discarded: the worker
   * could not hold the message (§5.3); the transfer was completed, and body is only its first
   * bytes (at most 8).
   */
  static final class Message {
    int op;
    int depth;
    long callSeq;
    MemorySegment body;
    long length;
    boolean discarded;

    int op() {
      return op;
    }

    int depth() {
      return depth;
    }

    long callSeq() {
      return callSeq;
    }

    boolean discarded() {
      return discarded;
    }
  }

  private final Message message = new Message();

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
    this.hostBody = region.asSlice(Wire.OFF_TO_W + Wire.LINE_BODY, Wire.INLINE_BODY);
    this.workerBody = region.asSlice(Wire.OFF_TO_H + Wire.LINE_BODY, Wire.INLINE_BODY);
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

  /**
   * A mailbox in memory that no host reads, for the AOT training run (Train): what is posted to it
   * stays there, and a request is put in by hand (Train.request).
   */
  static Mailbox inMemory(int arenaSize) {
    return new Mailbox(Arena.global().allocate(Wire.HEADER_SIZE + arenaSize, 4096), arenaSize, Wire.VERSION, 0);
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

  /**
   * Waits up to nanos for a message from the host as waitHost() waits for one (spin, then sleep on
   * the futex, so that a message wakes it at once), without taking it; true if one came.
   */
  boolean awaitMessage(long nanos) {
    for (int i = 0; i < spin; i++) {
      if (pending()) return true;
      Thread.onSpinWait();
    }
    long end = System.nanoTime() + nanos;
    while (true) {
      INT.setVolatile(region, Wire.OFF_W_SLEEP, 1);
      int v = (int) INT.getVolatile(region, Wire.OFF_TO_W);
      long left = end - System.nanoTime();
      if (v == seenW && left > 0) Native.futexWait(toW, seenW, left);
      INT.setRelease(region, Wire.OFF_W_SLEEP, 0);
      if (pending()) return true;
      if (end - System.nanoTime() <= 0) return false;
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
   * Posts the header and body written in place (§4.1, §4.2): the new count is stored into the
   * worker's word, and that store and the load of the host's sleep flag are sequentially consistent.
   */
  private void post() {
    INT.setVolatile(region, Wire.OFF_TO_H, ++posts);
    if ((int) INT.getVolatile(region, Wire.OFF_H_SLEEP) != 0) Native.futexWake(toH);
  }

  /** The worker's header in the worker line (§5.1); the values are legal, so they fit. */
  private void header(int op, int status, int flags, int depth, long len, long total, long seq) {
    writeHeader(region, Wire.OFF_TO_H, op, status, flags, depth, len, total, seq);
  }

  /** A header into the line at line (also Train's, as the host). */
  static void writeHeader(MemorySegment region, long line, int op, int status, int flags, int depth, long len,
      long total, long seq) {
    region.set(ValueLayout.JAVA_BYTE, line + Wire.LINE_OP, (byte) op);
    region.set(ValueLayout.JAVA_BYTE, line + Wire.LINE_STATUS, (byte) status);
    region.set(ValueLayout.JAVA_BYTE, line + Wire.LINE_FLAGS, (byte) flags);
    region.set(ValueLayout.JAVA_BYTE, line + Wire.LINE_DEPTH, (byte) depth);
    region.set(ValueLayout.JAVA_INT, line + Wire.LINE_LEN, (int) len);
    region.set(ValueLayout.JAVA_INT, line + Wire.LINE_TOTAL_LEN, (int) total);
    region.set(ValueLayout.JAVA_LONG, line + Wire.LINE_CALL_SEQ, seq);
  }

  /** A byte field of the host's header, unsigned. */
  private int hostByte(long field) {
    return Byte.toUnsignedInt(region.get(ValueLayout.JAVA_BYTE, Wire.OFF_TO_W + field));
  }

  /** A u32 field of the host's header. */
  private long hostU32(long field) {
    return Integer.toUnsignedLong(region.get(ValueLayout.JAVA_INT, Wire.OFF_TO_W + field));
  }

  private static boolean hostOp(int op) {
    return switch (op) {
      case Wire.PREPARE, Wire.CALL, Wire.SQL_RESULT, Wire.DROP, Wire.RESET, Wire.OPTIONS, Wire.PARSE_LIBRARY,
           Wire.LIBRARY_RESULT -> true;
      default -> false;
    };
  }

  // ------------------------------------------------------------ messages

  private Message message(int op, int depth, long seq, MemorySegment body, long length, boolean discarded) {
    Message m = message;
    m.op = op;
    m.depth = depth;
    m.callSeq = seq;
    m.body = body;
    m.length = length;
    m.discarded = discarded;
    return m;
  }

  /**
   * Waits for the next message of the host and reassembles it if it comes in chunks (§5.2). Of
   * the first chunk the operation and the admission are checked before anything is answered, of
   * every chunk its len: at most the arena and the bytes still due. A message of more bytes than
   * a host message may have, or than the worker can allocate, is received to its end without its
   * bytes (§5.3).
   */
  Message receive(Admission admission) {
    waitHost();
    int op = hostByte(Wire.LINE_OP);
    int flags = hostByte(Wire.LINE_FLAGS);
    int depth = hostByte(Wire.LINE_DEPTH);
    long len = hostU32(Wire.LINE_LEN);
    long total = hostU32(Wire.LINE_TOTAL_LEN);
    long seq = region.get(ValueLayout.JAVA_LONG, Wire.OFF_TO_W + Wire.LINE_CALL_SEQ);
    if (dropAssembly) {  // a large message's buffer is not kept for the worker's life
      assembly = MemorySegment.NULL;
      dropAssembly = false;
    }
    if (!hostOp(op)) throw ProtocolFailure.exit("op " + op + " from the host");
    boolean more = (flags & Wire.FLAG_MORE) != 0;
    if (len > arenaSize) throw ProtocolFailure.exit("a chunk of op " + op + " of " + len + " bytes");
    // An inline message is one chunk (§5.1).
    if (total <= Wire.INLINE_BODY && (more || len != total))
      throw ProtocolFailure.exit("an inline message of op " + op + " in chunks");
    admission.check(op, depth, seq);
    if (!more) return message(op, depth, seq, total <= Wire.INLINE_BODY ? hostBody : arena, len, false);

    boolean discard = total > Wire.MAX_HOST_MESSAGE;
    if (!discard && assembly.byteSize() < total) {
      try {
        assembly = Arena.ofAuto().allocate(total, Wire.VECTOR_ALIGNMENT);
      } catch (OutOfMemoryError e) {
        discard = true;
      }
    }
    MemorySegment head = MemorySegment.NULL;
    long headLength = Math.min(HEAD, len);
    if (discard) head = MemorySegment.ofArray(arena.asSlice(0, headLength).toArray(ValueLayout.JAVA_BYTE));
    else dropAssembly = assembly.byteSize() > 4L * arenaSize;
    long have = 0;
    while (true) {
      // The bytes still due bound a chunk; one that is discarded has nowhere to overflow.
      if (!discard) {
        if (len > total - have)
          throw ProtocolFailure.exit("bad chunk of op " + op + ": len " + len + " after " + have + " of " + total);
        MemorySegment.copy(arena, 0, assembly, have, len);
      }
      have += len;
      if (!more) break;
      header(Wire.CONTINUE, 0, 0, depth, 0, 0, seq);
      post();
      waitHost();
      if (hostByte(Wire.LINE_OP) != op)
        throw ProtocolFailure.exit("op " + hostByte(Wire.LINE_OP) + " among the chunks of op " + op);
      more = (hostByte(Wire.LINE_FLAGS) & Wire.FLAG_MORE) != 0;
      len = hostU32(Wire.LINE_LEN);
      if (len > arenaSize) throw ProtocolFailure.exit("a chunk of op " + op + " of " + len + " bytes");
    }
    return discard ? message(op, depth, seq, head, headLength, true) : message(op, depth, seq, assembly, have, false);
  }

  /**
   * Posts the message in w (written in place, or spilled and sent in chunks). The writer keeps
   * the size limits of §12; between the chunks the host posts CONTINUE and nothing else. flags:
   * FLAG_TAINTED or 0, the same in every chunk.
   */
  void send(int op, int status, int flags, int depth, long seq, MessageWriter w) {
    long total = w.size();
    if (!w.spilled()) {
      // Written in the arena: a body of at most 40 bytes moves to the worker line (§5.1).
      if (total <= Wire.INLINE_BODY && total > 0) MemorySegment.copy(arena, 0, workerBody, 0, total);
      header(op, status, flags, depth, total, total, seq);
      post();
      return;
    }
    MemorySegment body = w.body();
    long sent = 0;
    while (true) {
      long len = Math.min(arenaSize, total - sent);
      MemorySegment.copy(body, sent, arena, 0, len);
      boolean more = sent + len < total;
      header(op, status, more ? flags | Wire.FLAG_MORE : flags, depth, len, total, seq);
      post();
      sent += len;
      if (!more) return;
      waitHost();
      if (hostByte(Wire.LINE_OP) != Wire.CONTINUE) throw ProtocolFailure.exit("expected CONTINUE for op " + op);
    }
  }

  // ------------------------------------------------------------ stats

  /**
   * Writes the statistics (§3.1): for reporting only, and not on every request (MemoryWatch's
   * thread writes them while calls run, the main thread at READY and after RESET). Any thread;
   * nothing is synchronized. A heap without a limit has heap_max 0.
   */
  void stats(int state, long heapUsed, long heapMax) {
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_USED, heapUsed);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_HEAP_MAX, heapMax == Long.MAX_VALUE ? 0 : heapMax);
    region.set(ValueLayout.JAVA_LONG, Wire.OFF_STATS + Wire.STATS_GC_COUNT, MemoryWatch.gcs());
    region.set(ValueLayout.JAVA_INT, Wire.OFF_STATS + Wire.STATS_STATE, state);
  }

  void stats(int state) {
    Runtime rt = Runtime.getRuntime();
    stats(state, rt.totalMemory() - rt.freeMemory(), rt.maxMemory());
  }
}
