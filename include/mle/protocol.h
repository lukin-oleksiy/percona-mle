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

#ifndef MLE_PROTOCOL_H
#define MLE_PROTOCOL_H

/**
  @file components/mle/include/mle/protocol.h

  Wire protocol v1 between the MLE host (mysqld) and an MLE worker process.

  This header is the single definition of the protocol. It is shared by the
  host library, the launcher and the worker bootstrap; the Java worker
  (com.percona.mle.worker) mirrors these constants and must be kept in sync.
  No MySQL headers are used here.

  Transport
  ---------
  One shared memory region per worker (a memfd created by the launcher):
  a 4 KiB header (RegionHeader) followed by the arena that carries one
  message at a time. The two sides strictly alternate: the host writes a
  message into the arena, fills RegionHeader::msg and increments to_w; the
  worker answers the same way through to_h. Each side spins for a while and
  then sleeps on the futex word of the other side; the *_sleep flags tell
  the poster whether a FUTEX_WAKE is needed.

  The worker is untrusted. The host copies the message header and the
  message body out of shared memory before it validates or decodes them,
  and any violation ends the worker.

  Out of band, a SOCK_SEQPACKET control socket carries INTERRUPT and
  SHUTDOWN from the host; its EOF tells the worker that the host is gone.

  Encoding
  --------
  Little-endian. Integers have fixed width. A string or byte string is a
  u32 length followed by the bytes. Strings are well-formed UTF-8 (the host
  converts character sets). A value is a u8 tag followed by its payload,
  see Tag. The message bodies are described next to Op.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mle::wire {

/** File descriptors a worker process starts with (besides 0, 1, 2, which
    are /dev/null or inherited for debugging): the shared memory region and
    its end of the control socket. */
constexpr int kMailboxFd = 3;
constexpr int kControlFd = 4;

/** "MLE1" read as a little-endian u32. */
constexpr uint32_t kMagic = 0x31454c4d;
constexpr uint32_t kVersion = 1;

constexpr size_t kCacheLine = 64;
/** Size of RegionHeader; the arena starts here (page aligned). */
constexpr size_t kHeaderSize = 4096;
constexpr uint32_t kDefaultArenaSize = 1U << 20;
constexpr uint32_t kMinArenaSize = 64U << 10;
/** Upper bound of one logical message (all chunks together). */
constexpr uint64_t kMaxMessageSize = 1ULL << 30;
/** Default limit of nested calls (routine -> SQL -> routine ...). */
constexpr uint32_t kDefaultMaxDepth = 32;
/** Same limit as MySQL's VECTOR type. */
constexpr uint32_t kMaxVectorDimensions = 16383;
/** Same limit as MySQL's TIME type. */
constexpr uint32_t kMaxTimeHour = 838;
/** Oracle MLE limit for stack traces. */
constexpr uint32_t kMaxStackFrames = 1000;
/** VECTOR data starts at this alignment, relative to the start of the
    message; that is the arena start when the message fits in one chunk. */
constexpr size_t kVectorAlignment = 16;

/** Mailbox operations. Comments give the direction and the body. */
enum class Op : uint32_t {
  /** w->h. u32 protocol version, str worker class, u32 n + n x str
      languages, str runtime version, str info. With a status other than OK
      the worker failed to start and info says why. */
  kReady = 1,
  /** h->w. u64 routine id, str language, str name (the routine's name, for
      stack traces and messages), str entry, str source, u32 n + n x Param,
      u8 has_return [, ParamType return type]. An empty entry means the
      source is a routine *body* (CREATE ... AS $$ body $$): the worker
      wraps it with the parameters as named variables (Oracle's form for
      JavaScript). Answered by kResult (OK, or kGuestError with the compile
      error). */
  kPrepare = 2,
  /** h->w. u64 routine id, u32 events (Event bits), u32 n + n x value. */
  kCall = 3,
  /** w->h. u8 has_return [, value], u32 n + n x (u32 parameter index,
      value) for OUT and INOUT parameters, str stdout, str stderr,
      u8 has_error [, Error]. */
  kResult = 4,
  /** w->h. str statement, u32 n + n x value (parameters), u32 max rows per
      chunk (0 = host default). */
  kSql = 5,
  /** h->w. u8 SqlResultKind, then
      kError: u32 errno, str sqlstate, str message;
      kOk:    u64 affected rows, u64 last insert id, warnings;
      kRows:  u32 n + n x Column, u32 rows + rows x columns x value,
              u8 more rows, warnings.
      warnings = u32 n + n x (u32 level, u32 code, str message). */
  kSqlResult = 6,
  /** w->h. u32 max rows: the next chunk of the current result set. */
  kFetch = 7,
  /** h->w. u64 routine id: forget the routine. Answered by kResult. */
  kDrop = 8,
  /** h->w. Empty: forget all routines and guest state (mle_session_reset).
      Answered by kResult. */
  kReset = 9,
  /** both. Empty: acknowledges a chunk that had MessageFlag::kMore. */
  kContinue = 10,
  /** h->w. u8 IntegerType, u8 DecimalType, str time zone (IANA name or
      +hh:mm): the session's conversion options (mle_set_session_state) and
      time zone. Sent before the first CALL and whenever they change.
      Answered by kResult (kGuestError with language "MLE-TimeZone" for a
      zone the worker does not know). */
  kOptions = 11,
};
constexpr uint32_t kMaxOp = 11;

/** How JavaScript gets SQL integers (Oracle's integer_type option). A safe
    integer is within +-(2^53 - 1). */
enum class IntegerType : uint8_t {
  /** Number if safe, else String (Oracle's default). */
  kUnsafeString = 0,
  /** Number if safe, else BigInt. */
  kUnsafeBigint = 1,
  /** Always String. */
  kString = 2,
  /** Always BigInt. */
  kBigint = 3,
};
constexpr uint8_t kMaxIntegerType = 3;

/** How JavaScript gets DECIMAL (Oracle's decimal_type option). */
enum class DecimalType : uint8_t { kString = 0, kNumber = 1 };
constexpr uint8_t kMaxDecimalType = 1;

/** RESULT status (MessageHeader::status). */
enum class Status : uint32_t {
  kOk = 0,
  kGuestError = 1,
  kInterrupted = 2,
  /** Heap or stack exhausted in the worker. */
  kExhausted = 3,
  /** Bug or failure inside the worker runtime. */
  kInternal = 4,
  /** Host side only: the worker exited or was killed by a signal. */
  kDied = 5,
  /** Host side only: the worker was killed after the interrupt grace. */
  kKilled = 6,
  /** Host side only: the worker broke the protocol and was killed. */
  kProtocolError = 7,
};
/** Highest status a worker may send. */
constexpr uint32_t kMaxWorkerStatus = 4;

/** MessageHeader::flags */
enum MessageFlag : uint32_t {
  /** More chunks of this message follow; the receiver answers kContinue. */
  kMore = 1U << 0,
};
constexpr uint32_t kKnownMessageFlags = kMore;

/** CALL events: what happened in the session since the previous CALL to the
    same worker (Informix "durations"). */
enum Event : uint32_t {
  kStatementEnd = 1U << 0,
  kTransactionCommit = 1U << 1,
  kTransactionRollback = 1U << 2,
};
constexpr uint32_t kKnownEvents =
    kStatementEnd | kTransactionCommit | kTransactionRollback;

/** Value tags. Payloads:
    kNull      -
    kI64/kU64  8 bytes
    kF64/kF32  8/4 bytes, IEEE 754
    kDecimal   str, [-]digits[.digits]
    kString    str (UTF-8)
    kBytes     u32 length + bytes
    kDate .. kTimestamp  Temporal: u16 year, u8 month, u8 day, u32 hour,
               u8 minute, u8 second, u32 microsecond, u8 negative
    kJson      str (JSON text)
    kVector    u32 dimensions, zero padding up to kVectorAlignment relative
               to the message start, dimensions x float32 */
enum class Tag : uint8_t {
  kNull = 0,
  kI64 = 1,
  kU64 = 2,
  kF64 = 3,
  kF32 = 4,
  kDecimal = 5,
  kString = 6,
  kBytes = 7,
  kDate = 8,
  kTime = 9,
  kDatetime = 10,
  kTimestamp = 11,
  kJson = 12,
  kVector = 13,
};
constexpr uint8_t kMaxTag = 13;

/** Parameter mode in PREPARE. */
enum class ParamMode : uint8_t { kIn = 0, kOut = 1, kInOut = 2 };

/** ParamType = u8 tag, u8 unsigned, u32 length, u32 precision, u32 scale,
            str SQL type name (upper case, without length: INT, VARCHAR,
            SET, YEAR, BIT ...; empty if unknown).
    Param = str name, u8 mode, ParamType.
    Column = str name, str table, ParamType.
    Error = str language, str message, u32 n + n x (str function, str file,
            u32 line, u32 column, u32 end column) stack frames, u32 sql
            errno (0 = not a MySQL error), str sqlstate (empty or 5
            characters). The language "MLE-Type" marks a value conversion
            error (Oracle: 6000 "MLE-Type> Cannot convert value ..."). */

enum class SqlResultKind : uint8_t { kError = 0, kOk = 1, kRows = 2 };

/** Control socket messages, h->w. INTERRUPT carries the call sequence of the
    call to interrupt: u8 kind + u64 call_seq; SHUTDOWN is u8 kind only. */
enum class Control : uint8_t { kInterrupt = 1, kShutdown = 2 };

/** Worker state in WorkerStats::state (advisory). */
enum class WorkerState : uint32_t {
  kStarting = 0,
  kIdle = 1,
  kRunning = 2,
  kCompiling = 3,
};

/** The header of the message currently in the arena. Written by the poster
    before it bumps its sequence word. */
struct MessageHeader {
  uint32_t op;
  uint32_t status;
  uint32_t flags;
  /** Nesting level: 0 for a top-level CALL, +1 for each CALL sent while
      the worker waits for a SQL_RESULT. */
  uint32_t depth;
  /** Bytes of this chunk in the arena. */
  uint32_t len;
  uint32_t reserved;
  /** Total bytes of the message over all its chunks. */
  uint64_t total_len;
  /** Sequence number of the top-level call this message belongs to. */
  uint64_t call_seq;
};
static_assert(sizeof(MessageHeader) == 40);

/** Written by the worker after each message; the host treats it as advisory
    and clamps what it reports. */
struct WorkerStats {
  uint64_t heap_used;
  uint64_t heap_max;
  uint64_t gc_count;
  uint32_t state;
  /** Non-zero: the worker hit OutOfMemoryError or StackOverflowError and
      asks to be retired after the current call. */
  uint32_t tainted;
};
static_assert(sizeof(WorkerStats) == 32);

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

/** Layout of the first kHeaderSize bytes of the region. One cache line per
    field group, so that the two sides do not share lines they write. */
struct alignas(kCacheLine) RegionHeader {
  // Line 0, written once by the launcher.
  uint32_t magic;
  uint32_t version;
  uint64_t region_size;
  uint32_t arena_size;
  uint32_t reserved0;
  char pad0[kCacheLine - 24];
  // Line 1: host -> worker sequence (futex word).
  std::atomic<uint32_t> to_w;
  char pad1[kCacheLine - 4];
  // Line 2: worker -> host sequence (futex word).
  std::atomic<uint32_t> to_h;
  char pad2[kCacheLine - 4];
  // Line 3: sleep flags of the spin-then-futex wait.
  std::atomic<uint32_t> w_sleep;
  std::atomic<uint32_t> h_sleep;
  char pad3[kCacheLine - 8];
  // Line 4.
  MessageHeader msg;
  char pad4[kCacheLine - sizeof(MessageHeader)];
  // Line 5.
  WorkerStats stats;
  char pad5[kCacheLine - sizeof(WorkerStats)];
  char reserved[kHeaderSize - 6 * kCacheLine];
};

static_assert(sizeof(RegionHeader) == kHeaderSize);
static_assert(offsetof(RegionHeader, to_w) == 1 * kCacheLine);
static_assert(offsetof(RegionHeader, to_h) == 2 * kCacheLine);
static_assert(offsetof(RegionHeader, w_sleep) == 3 * kCacheLine);
static_assert(offsetof(RegionHeader, h_sleep) == 3 * kCacheLine + 4);
static_assert(offsetof(RegionHeader, msg) == 4 * kCacheLine);
static_assert(offsetof(RegionHeader, stats) == 5 * kCacheLine);

inline constexpr size_t region_size(uint32_t arena_size) {
  return kHeaderSize + arena_size;
}

}  // namespace mle::wire

#endif  // MLE_PROTOCOL_H
