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

/**
 * The constants of the MLE worker protocol, specification 1.0 (docs/protocol.md; the section
 * numbers in the comments are its). include/mle/protocol.h has the same for the C++ side.
 */
final class Wire {
  private Wire() {}

  // The descriptors a worker starts with (§2).
  static final int MAILBOX_FD = 3;
  static final int CONTROL_FD = 4;
  /** The exit status of a worker that cannot use the region (§2). */
  static final int EXIT_BAD_REGION = 2;
  /** The exit status after a protocol failure of the host (§1). */
  static final int EXIT_PROTOCOL_FAILURE = 3;

  static final int MAGIC = 0x31454c4d;
  /** version = major << 16 | minor (§14). */
  static final int VERSION_MAJOR = 1;
  static final int VERSION = VERSION_MAJOR << 16;
  static final long HEADER_SIZE = 4096;

  // Limits (§12).
  static final long MIN_ARENA_SIZE = 64L << 10;
  static final long MAX_ARENA_SIZE = 1L << 30;
  /** A message of the host, all chunks together. */
  static final long MAX_HOST_MESSAGE = 1L << 30;
  /** A message of the worker, all chunks together. */
  static final long MAX_WORKER_MESSAGE = 64L << 20;
  /** The messages of one chunk: READY, LIBRARY, the RESULT of DROP, RESET and OPTIONS; OPTIONS, DROP, RESET. */
  static final long MAX_SMALL_MESSAGE = 64L << 10;
  /** The deepest nested request (§8.2). */
  static final int MAX_DEPTH = 31;
  /** Values in one list: arguments, SQL parameters, OUT parameters. */
  static final int MAX_VALUES = 65535;
  static final int MAX_PARAMS = 65535;
  static final int MAX_IMPORTS = 65535;
  static final int MAX_COLUMNS = 4096;
  static final int MAX_WARNINGS = 65535;
  static final int MAX_SETTINGS = 256;
  static final int MAX_SETTING_NAME = 64;
  static final int MAX_SETTING_VALUE = 1024;
  static final int MAX_TIME_ZONE = 64;
  /** A language identifier or a worker class. */
  static final int MAX_IDENTIFIER = 32;
  static final int MAX_LANGUAGES = 16;
  static final int MAX_ERROR_MESSAGE = 16 << 10;
  /** Frame function, file and text, each. */
  static final int MAX_FRAME_TEXT = 1 << 10;
  static final int MAX_STACK_FRAMES = 1000;
  static final int MAX_FINDING = 4 << 10;
  static final int MAX_FINDINGS = 1000;
  /** LIBRARY schema and name, each. */
  static final int MAX_LIBRARY_NAME = 256;
  static final int MAX_DECIMAL_CHARS = 100;
  static final int MAX_VECTOR_DIMENSIONS = 16383;
  static final int MAX_TIME_HOUR = 838;
  static final int MAX_SQL_ERRNO = 65535;
  /** VECTOR elements start at this alignment, counted from the start of the message body (§7.2). */
  static final int VECTOR_ALIGNMENT = 16;

  /** TIMESTAMP (§7.2): microseconds since 1970-01-01 00:00:00 UTC, up to this. */
  static final long MAX_TIMESTAMP = 253402300799999999L;
  /** MySQL's zero timestamp. */
  static final long ZERO_TIMESTAMP = Long.MIN_VALUE;
  /** SQL_RESULT ROWS total_rows when the host does not know it (§10.3): 2^64 - 1. */
  static final long UNKNOWN_TOTAL_ROWS = -1L;

  // The region (§3).
  static final long OFF_MAGIC = 0;
  static final long OFF_VERSION = 4;
  static final long OFF_REGION_SIZE = 8;
  static final long OFF_ARENA_SIZE = 16;
  static final long OFF_TO_W = 64;
  static final long OFF_TO_H = 128;
  static final long OFF_W_SLEEP = 192;
  static final long OFF_H_SLEEP = 256;
  static final long OFF_MSG = 320;
  static final long OFF_STATS = 384;
  // The message header (§5.1), relative to OFF_MSG.
  static final long MSG_OP = 0;
  static final long MSG_STATUS = 4;
  static final long MSG_FLAGS = 8;
  static final long MSG_DEPTH = 12;
  static final long MSG_LEN = 16;
  static final long MSG_RESERVED = 20;
  static final long MSG_TOTAL_LEN = 24;
  static final long MSG_CALL_SEQ = 32;
  // The worker statistics (§3.1), relative to OFF_STATS.
  static final long STATS_HEAP_USED = 0;
  static final long STATS_HEAP_MAX = 8;
  static final long STATS_GC_COUNT = 16;
  static final long STATS_STATE = 24;
  static final long STATS_TAINTED = 28;

  // Operations (§9). 0, 7 and above 14 are not assigned.
  static final int READY = 1;
  static final int PREPARE = 2;
  static final int CALL = 3;
  static final int RESULT = 4;
  static final int SQL = 5;
  static final int SQL_RESULT = 6;
  static final int DROP = 8;
  static final int RESET = 9;
  static final int CONTINUE = 10;
  static final int OPTIONS = 11;
  static final int PARSE_LIBRARY = 12;
  static final int LIBRARY = 13;
  static final int LIBRARY_RESULT = 14;

  // LIBRARY_RESULT status (§9.10).
  static final int LIBRARY_REFUSED = 0;
  static final int LIBRARY_UNCHANGED = 1;
  static final int LIBRARY_BODY = 2;

  // Error.language markers (§7.4).
  static final String LANGUAGE_TYPE = "MLE-Type";
  static final String LANGUAGE_TIME_ZONE = "MLE-TimeZone";
  static final String LANGUAGE_OPTION = "MLE-Option";

  // The status of RESULT (§9.4) and READY (§9.1: OK or INTERNAL).
  static final int OK = 0;
  static final int GUEST_ERROR = 1;
  static final int INTERRUPTED = 2;
  static final int EXHAUSTED = 3;
  static final int INTERNAL = 4;

  static final int FLAG_MORE = 1;

  // Value tags (§7.2).
  static final int T_NULL = 0;
  static final int T_I64 = 1;
  static final int T_U64 = 2;
  static final int T_F64 = 3;
  static final int T_F32 = 4;
  static final int T_DECIMAL = 5;
  static final int T_STRING = 6;
  static final int T_BYTES = 7;
  static final int T_DATE = 8;
  static final int T_TIME = 9;
  static final int T_DATETIME = 10;
  static final int T_TIMESTAMP = 11;
  static final int T_JSON = 12;
  static final int T_VECTOR = 13;
  static final int MAX_TAG = 13;

  static final String[] TAG_NAMES = {
    "NULL", "BIGINT", "BIGINT UNSIGNED", "DOUBLE", "FLOAT", "DECIMAL", "string", "binary",
    "DATE", "TIME", "DATETIME", "TIMESTAMP", "JSON", "VECTOR"
  };

  // Param.mode (§7.4).
  static final int MODE_IN = 0;
  static final int MODE_OUT = 1;
  static final int MODE_INOUT = 2;

  // SQL_RESULT kind (§9.5).
  static final int SQL_ERROR = 0;
  static final int SQL_OK = 1;
  static final int SQL_ROWS = 2;
  static final int SQL_END = 3;

  // SQL action and flags (§10.2).
  static final int SQL_EXECUTE = 0;
  static final int SQL_PREPARE = 1;
  static final int SQL_EXECUTE_PREPARED = 2;
  static final int SQL_FETCH = 3;
  static final int SQL_NEXT_RESULT = 4;
  static final int SQL_CLOSE = 5;
  static final int SQL_EXECUTE_MANY = 6;
  /** SQL_RESULT ERROR failed_row when the error does not answer EXECUTE_MANY. */
  static final long NO_ROW = 0xFFFFFFFFL;
  static final int SQL_FLAG_TO_CLIENT = 1;

  // The control socket (§6): INTERRUPT is this byte and a u64 call_seq.
  static final int CTL_INTERRUPT = 1;
  static final int CTL_INTERRUPT_SIZE = 9;

  // WorkerStats.state (§3.1).
  static final int STATE_IDLE = 1;
  static final int STATE_RUNNING = 2;
  static final int STATE_COMPILING = 3;
}
