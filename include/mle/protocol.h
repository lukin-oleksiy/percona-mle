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
  @file include/mle/protocol.h

  The constants and layouts of the MLE worker protocol, specification 1.0.

  The protocol is defined by docs/protocol.md; section numbers in the
  comments below refer to it. This header is shared by the host library,
  the launcher and the workers written in C++; the Java worker mirrors it.
  No MySQL headers are used here.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mle::wire {

/** The descriptors a worker starts with (§2). */
constexpr int kMailboxFd = 3;
constexpr int kControlFd = 4;
/** A worker's exit status when it cannot use the region (§2). */
constexpr int kExitBadRegion = 2;

/** "MLE1" read as a little-endian u32 (§3). */
constexpr uint32_t kMagic = 0x31454c4d;
/** version = major << 16 | minor (§14). */
constexpr uint32_t kVersionMajor = 1;
constexpr uint32_t kVersionMinor = 0;
constexpr uint32_t kVersion = kVersionMajor << 16 | kVersionMinor;
constexpr uint32_t version_major(uint32_t version) { return version >> 16; }
constexpr uint32_t version_minor(uint32_t version) { return version & 0xFFFF; }

constexpr size_t kCacheLine = 64;
/** Size of RegionHeader; the arena starts here. */
constexpr size_t kHeaderSize = 4096;

// Limits (§12).
constexpr uint32_t kDefaultArenaSize = 1U << 20;
constexpr uint32_t kMinArenaSize = 64U << 10;
constexpr uint32_t kMaxArenaSize = 1U << 30;
constexpr uint32_t kArenaGranule = 4096;
/** A message of the host, all chunks together. */
constexpr uint64_t kMaxHostMessage = 1ULL << 30;
/** A message of a worker, all chunks together. */
constexpr uint64_t kMaxWorkerMessage = 64ULL << 20;
/** The messages limited to one chunk: READY, LIBRARY, the RESULT of DROP,
    RESET and OPTIONS; OPTIONS, DROP and RESET themselves. */
constexpr uint64_t kMaxSmallMessage = 64ULL << 10;
static_assert(kMaxSmallMessage <= kMinArenaSize);
/** The deepest nested request (§8.2). */
constexpr uint32_t kMaxDepth = 31;
/** Values in one list: arguments, SQL parameters, OUT parameters. */
constexpr uint32_t kMaxValues = 65535;
constexpr uint32_t kMaxParams = 65535;
constexpr uint32_t kMaxImports = 65535;
constexpr uint32_t kMaxColumns = 4096;
constexpr uint32_t kMaxWarnings = 65535;
constexpr uint32_t kMaxSettings = 256;
constexpr size_t kMaxSettingName = 64;
constexpr size_t kMaxSettingValue = 1024;
constexpr size_t kMaxTimeZone = 64;
/** A language identifier or a worker class. */
constexpr size_t kMaxIdentifier = 32;
constexpr uint32_t kMaxLanguages = 16;
constexpr size_t kMaxErrorMessage = 16U << 10;
/** Frame function, file and text, each. */
constexpr size_t kMaxFrameText = 1U << 10;
constexpr uint32_t kMaxStackFrames = 1000;
constexpr size_t kMaxFinding = 4U << 10;
constexpr uint32_t kMaxFindings = 1000;
/** LIBRARY schema and name, each. */
constexpr size_t kMaxLibraryName = 256;
constexpr size_t kMaxDecimalChars = 100;
constexpr uint32_t kMaxVectorDimensions = 16383;
constexpr uint32_t kMaxTimeHour = 838;
constexpr uint32_t kMaxSqlErrno = 65535;
/** The ERRORs the host holds per top-level request and worker, at least
    (§10.8). */
constexpr uint32_t kHeldErrors = 16;
/** VECTOR elements start at this alignment, counted from the start of the
    message body (§7.2). */
constexpr size_t kVectorAlignment = 16;
/** The size that adds one grace period to a bound of the grace clock
    (§11.1 step 3). */
constexpr uint64_t kGraceBytes = 16ULL << 20;

/** TIMESTAMP (§7.2): microseconds since 1970-01-01 00:00:00 UTC. */
constexpr int64_t kMaxTimestamp = 253402300799999999LL;
/** MySQL's zero timestamp. */
constexpr int64_t kZeroTimestamp = INT64_MIN;
/** SQL_RESULT ROWS total_rows when the host does not know it (§10.3). */
constexpr uint64_t kUnknownTotalRows = UINT64_MAX;

/** Operations (§9). 0, 7 and above 14 are not assigned. */
enum class Op : uint32_t {
  kReady = 1,
  kPrepare = 2,
  kCall = 3,
  kResult = 4,
  kSql = 5,
  kSqlResult = 6,
  kDrop = 8,
  kReset = 9,
  kContinue = 10,
  kOptions = 11,
  kParseLibrary = 12,
  kLibrary = 13,
  kLibraryResult = 14,
};
constexpr uint32_t kMaxOp = 14;
constexpr bool is_assigned_op(uint32_t op) {
  return op >= 1 && op <= kMaxOp && op != 7;
}

/** LIBRARY_RESULT status (§9.10). */
enum class LibraryStatus : uint8_t { kRefused = 0, kUnchanged = 1, kBody = 2 };
constexpr uint8_t kMaxLibraryStatus = 2;

/** MessageHeader::status of RESULT (§9.4) and READY (§9.1: kOk or kInternal).
    Every other message has 0. What a request ends with in the host besides
    these is mle::Outcome (worker.h). */
enum class Status : uint32_t {
  kOk = 0,
  kGuestError = 1,
  kInterrupted = 2,
  kExhausted = 3,
  kInternal = 4,
};
constexpr uint32_t kMaxStatus = 4;

/** Error.language markers (§7.4). */
inline constexpr char kLanguageType[] = "MLE-Type";
inline constexpr char kLanguageTimeZone[] = "MLE-TimeZone";
inline constexpr char kLanguageOption[] = "MLE-Option";

/** MessageHeader::flags (§5.1). kTainted is legal only in a RESULT, where
    all chunks of the message carry it alike: the worker asks to be retired
    (§11.5). */
enum MessageFlag : uint32_t {
  kMore = 1U << 0,
  kTainted = 1U << 1,
};
constexpr uint32_t kKnownMessageFlags = kMore | kTainted;

/** Value tags (§7.2). */
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

/** Param.mode (§7.4). */
enum class ParamMode : uint8_t { kIn = 0, kOut = 1, kInOut = 2 };

/** Warning.level (§7.4). */
enum class WarningLevel : uint32_t { kNote = 0, kWarning = 1, kError = 2 };

/** SQL_RESULT kind (§9.5). */
enum class SqlResultKind : uint8_t { kError = 0, kOk = 1, kRows = 2, kEnd = 3 };
constexpr uint8_t kMaxSqlResultKind = 3;

/** SQL action (§10.2). */
enum class SqlAction : uint8_t {
  kExecute = 0,
  kPrepare = 1,
  kExecutePrepared = 2,
  kFetch = 3,
  kNextResult = 4,
  kClose = 5,
  kExecuteMany = 6,
};
constexpr uint8_t kMaxSqlAction = 6;
/** The body of an SQL message begins with u8 action, u64 statement_id; a
    CLOSE has no other content (§5.3). */
constexpr size_t kSqlHeadSize = 9;
constexpr size_t kSqlCloseSize = 26;
/** SQL_RESULT ERROR failed_row when the error does not answer EXECUTE_MANY. */
constexpr uint32_t kNoRow = 0xFFFFFFFF;

/** SQL flags (§10.2). kKeep (a kept statement, §10.1) is not accepted by
    this host: the server's statement service binds a prepared statement to
    the stored program call that prepared it (a statement handle keeps that
    call's diagnostics area), so no statement can outlive its request. A
    worker that sends it commits a violation (an unknown flag). */
enum SqlFlag : uint8_t {
  kPassResultToClient = 1U << 0,
  kKeep = 1U << 1,
};
constexpr uint8_t kKnownSqlFlags = kPassResultToClient;

/** Control socket messages (§6): u8 kind, u64 call_seq. */
enum class Control : uint8_t { kInterrupt = 1 };
constexpr size_t kInterruptSize = 9;

/** WorkerStats::state (§3.1). */
enum class WorkerState : uint32_t {
  kStarting = 0,
  kIdle = 1,
  kRunning = 2,
  kCompiling = 3,
};

/** The message header (§5.1). */
struct MessageHeader {
  uint32_t op;
  uint32_t status;
  uint32_t flags;
  uint32_t depth;
  uint32_t len;
  uint32_t reserved;
  uint64_t total_len;
  uint64_t call_seq;
};
static_assert(sizeof(MessageHeader) == 40);

/** Worker statistics (§3.1): for reporting only. A worker writes them when
    they changed enough to matter and when it becomes idle after a RESET,
    not on every request; the host never decides anything by them. */
struct WorkerStats {
  uint64_t heap_used;
  uint64_t heap_max;
  uint64_t gc_count;
  uint32_t state;
  uint32_t reserved;
};
static_assert(sizeof(WorkerStats) == 32);

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

/** The first kHeaderSize bytes of the region (§3). */
struct alignas(kCacheLine) RegionHeader {
  // Line 0, written once by the launcher; for the worker.
  uint32_t magic;
  uint32_t version;
  uint64_t region_size;
  uint32_t arena_size;
  char reserved0[kCacheLine - 20];
  /** Sequence word, host to worker. */
  std::atomic<uint32_t> to_w;
  char pad1[kCacheLine - 4];
  /** Sequence word, worker to host. */
  std::atomic<uint32_t> to_h;
  char pad2[kCacheLine - 4];
  std::atomic<uint32_t> w_sleep;
  char pad3[kCacheLine - 4];
  std::atomic<uint32_t> h_sleep;
  char pad4[kCacheLine - 4];
  MessageHeader msg;
  char pad5[kCacheLine - sizeof(MessageHeader)];
  WorkerStats stats;
  char pad6[kCacheLine - sizeof(WorkerStats)];
  char reserved[kHeaderSize - 7 * kCacheLine];
};

static_assert(sizeof(RegionHeader) == kHeaderSize);
static_assert(offsetof(RegionHeader, region_size) == 8);
static_assert(offsetof(RegionHeader, arena_size) == 16);
static_assert(offsetof(RegionHeader, to_w) == 64);
static_assert(offsetof(RegionHeader, to_h) == 128);
static_assert(offsetof(RegionHeader, w_sleep) == 192);
static_assert(offsetof(RegionHeader, h_sleep) == 256);
static_assert(offsetof(RegionHeader, msg) == 320);
static_assert(offsetof(RegionHeader, stats) == 384);

inline constexpr size_t region_size(uint32_t arena_size) {
  return kHeaderSize + arena_size;
}
/** A legal arena size (§3). */
inline constexpr bool is_valid_arena_size(uint64_t arena_size) {
  return arena_size >= kMinArenaSize && arena_size <= kMaxArenaSize &&
         arena_size % kArenaGranule == 0;
}

}  // namespace mle::wire

#endif  // MLE_PROTOCOL_H
