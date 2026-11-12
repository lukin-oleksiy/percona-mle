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
 * Wire protocol v1 constants. Mirrors include/mle/protocol.h,
 * which is the definition; keep both in sync.
 */
final class Wire {
  private Wire() {}

  static final int MAILBOX_FD = 3;
  static final int CONTROL_FD = 4;

  static final int MAGIC = 0x31454c4d;
  static final int VERSION = 1;
  static final long HEADER_SIZE = 4096;
  static final int MAX_VECTOR_DIMENSIONS = 16383;
  static final int MAX_TIME_HOUR = 838;
  static final int MAX_STACK_FRAMES = 1000;
  static final int VECTOR_ALIGNMENT = 16;

  // RegionHeader offsets.
  static final long OFF_MAGIC = 0;
  static final long OFF_VERSION = 4;
  static final long OFF_REGION_SIZE = 8;
  static final long OFF_ARENA_SIZE = 16;
  static final long OFF_TO_W = 64;
  static final long OFF_TO_H = 128;
  static final long OFF_W_SLEEP = 192;
  static final long OFF_H_SLEEP = 196;
  static final long OFF_MSG = 256;
  static final long OFF_STATS = 320;
  // MessageHeader fields, relative to OFF_MSG.
  static final long MSG_OP = 0;
  static final long MSG_STATUS = 4;
  static final long MSG_FLAGS = 8;
  static final long MSG_DEPTH = 12;
  static final long MSG_LEN = 16;
  static final long MSG_TOTAL_LEN = 24;
  static final long MSG_CALL_SEQ = 32;
  // WorkerStats fields, relative to OFF_STATS.
  static final long STATS_HEAP_USED = 0;
  static final long STATS_HEAP_MAX = 8;
  static final long STATS_GC_COUNT = 16;
  static final long STATS_STATE = 24;
  static final long STATS_TAINTED = 28;

  // Op
  static final int READY = 1;
  static final int PREPARE = 2;
  static final int CALL = 3;
  static final int RESULT = 4;
  static final int SQL = 5;
  static final int SQL_RESULT = 6;
  static final int FETCH = 7;
  static final int DROP = 8;
  static final int RESET = 9;
  static final int CONTINUE = 10;
  static final int OPTIONS = 11;
  static final int PARSE_LIBRARY = 12;
  static final int LIBRARY = 13;
  static final int LIBRARY_RESULT = 14;
  static final int LIBRARY_REFUSED = 0;
  static final int LIBRARY_UNCHANGED = 1;
  static final int LIBRARY_BODY = 2;

  // IntegerType (Oracle's integer_type) and DecimalType (decimal_type)
  static final int INT_UNSAFE_STRING = 0;
  static final int INT_UNSAFE_BIGINT = 1;
  static final int INT_STRING = 2;
  static final int INT_BIGINT = 3;
  static final int DECIMAL_STRING = 0;
  static final int DECIMAL_NUMBER = 1;

  /** Error language of value conversion errors (Oracle: 6000 "MLE-Type> ..."). */
  static final String LANGUAGE_TYPE = "MLE-Type";
  static final String LANGUAGE_TIME_ZONE = "MLE-TimeZone";

  // Status
  static final int OK = 0;
  static final int GUEST_ERROR = 1;
  static final int INTERRUPTED = 2;
  static final int EXHAUSTED = 3;
  static final int INTERNAL = 4;

  static final int FLAG_MORE = 1;

  // Tag
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

  // ParamMode
  static final int MODE_IN = 0;
  static final int MODE_OUT = 1;
  static final int MODE_INOUT = 2;

  // SqlResultKind
  static final int SQL_ERROR = 0;
  static final int SQL_OK = 1;
  static final int SQL_ROWS = 2;

  // SqlAction, SqlFlag
  static final int SQL_EXECUTE = 0;
  static final int SQL_PREPARE = 1;
  static final int SQL_EXECUTE_PREPARED = 2;
  static final int SQL_FETCH = 3;
  static final int SQL_NEXT_RESULT = 4;
  static final int SQL_CLOSE = 5;
  static final int SQL_FLAG_TO_CLIENT = 1;

  // Control socket
  static final int CTL_INTERRUPT = 1;
  static final int CTL_SHUTDOWN = 2;

  // WorkerState
  static final int STATE_IDLE = 1;
  static final int STATE_RUNNING = 2;
  static final int STATE_COMPILING = 3;
}
