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

#ifndef MLE_CODEC_H
#define MLE_CODEC_H

/**
  @file include/mle/codec.h

  Encoding and decoding of the message bodies of the MLE worker protocol
  (docs/protocol.md §7 and §9), reassembly of chunked messages (§5.2) and the
  checks of a worker's message header (§13).

  Decoding treats its input as hostile: every length and count is checked
  against the bytes that are left and against the limits of §12, every value
  against its domain, and a body must be consumed exactly. A decoder reads
  each byte of its input once, front to back, and copies a field before it
  checks it, so it may decode a message in the region itself (§1). A decoder returns
  false and sets an error text on the first problem; it never reads outside
  its input. What a decoder refuses is malformed: from a worker, a violation.
  One thing a decoder accepts although the host must not use it: a float of
  a worker that is not finite (§7.2), which is an error and is found with
  all_finite().
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mle/protocol.h"
#include "mle/value.h"

namespace mle {

using wire::ParamMode;
using wire::SqlResultKind;

/** Who wrote the bytes a Reader decodes. Text values (lstr) of a worker
    must be well-formed UTF-8; the host's may hold the server's ill-formed
    text (§7.1). */
enum class Sender { kWorker, kHost };

/** Appends wire encodings to a byte string, or writes them into a fixed
    buffer (the arena). Offsets (for VECTOR padding) are counted from the
    start of the string or buffer, which is the start of the message body.
    In a fixed buffer, a write that does not fit sets overflow() and writes
    nothing; set_position() rolls back. */
class Writer {
 public:
  explicit Writer(std::string *out) : m_out(out) {}
  Writer(char *buffer, size_t capacity) : m_buf(buffer), m_cap(capacity) {}

  void u8(uint8_t v) { raw(&v, sizeof v); }
  void u16(uint16_t v) { raw(&v, sizeof v); }
  void u32(uint32_t v) { raw(&v, sizeof v); }
  void u64(uint64_t v) { raw(&v, sizeof v); }
  void i64(int64_t v) { raw(&v, sizeof v); }
  void f32(float v) { raw(&v, sizeof v); }
  void f64(double v) { raw(&v, sizeof v); }
  void flag(bool v) { u8(v ? 1 : 0); }
  /** str, lstr and bytes have one encoding. */
  void str(std::string_view s);
  void value(const Value &v);
  /** Values without an mle::Value, encoded exactly as value() encodes them. */
  void null_value() { u8(static_cast<uint8_t>(Tag::kNull)); }
  void i64_value(int64_t v);
  void u64_value(uint64_t v);
  void f64_value(double v);
  void f32_value(float v);
  /** kDecimal, kString, kBytes or kJson. */
  void text_value(Tag tag, std::string_view s);
  /** kDate, kTime or kDatetime. */
  void temporal_value(Tag tag, const Temporal &t);
  void timestamp_value(int64_t microseconds);
  /** dims floats from data (any alignment), after the padding. */
  void vector_value(const void *data, uint32_t dims);
  void raw(const void *p, size_t n) {
    if (m_out != nullptr) {
      m_out->append(static_cast<const char *>(p), n);
    } else if (m_overflow || n > m_cap - m_len) {
      m_overflow = true;
    } else {
      if (n > 0) memcpy(m_buf + m_len, p, n);
      m_len += n;
    }
  }
  size_t position() const { return m_out != nullptr ? m_out->size() : m_len; }
  /** Fixed buffer only: back to an earlier position, clearing overflow. */
  void set_position(size_t pos) {
    m_len = pos;
    m_overflow = false;
  }
  /** Fixed buffer only: a new capacity (at least position()). */
  void set_capacity(size_t capacity) { m_cap = capacity; }
  /** Fixed buffer only: overwrite a u32 written earlier at pos. */
  void patch_u32(size_t pos, uint32_t v) { memcpy(m_buf + pos, &v, sizeof v); }
  bool overflow() const { return m_overflow; }

 private:
  void zeros(size_t n);
  std::string *m_out = nullptr;
  char *m_buf = nullptr;
  size_t m_cap = 0;
  size_t m_len = 0;
  bool m_overflow = false;
};

/** Bounds-checked reader over one message body. All getters return false
    once anything failed; the first failure is kept in error(). */
class Reader {
 public:
  /** origin: the offset of data in the message body (offsets count from
      the body's start: VECTOR padding, error texts). */
  Reader(const void *data, size_t len, Sender from = Sender::kWorker,
         size_t origin = 0)
      : m_begin(static_cast<const unsigned char *>(data)),
        m_pos(m_begin),
        m_end(m_begin + len),
        m_from(from),
        m_origin(origin) {}
  explicit Reader(std::string_view s, Sender from = Sender::kWorker)
      : Reader(s.data(), s.size(), from) {}

  bool u8(uint8_t *v) { return fixed(v, sizeof *v); }
  bool u16(uint16_t *v) { return fixed(v, sizeof *v); }
  bool u32(uint32_t *v) { return fixed(v, sizeof *v); }
  bool u64(uint64_t *v) { return fixed(v, sizeof *v); }
  bool i64(int64_t *v) { return fixed(v, sizeof *v); }
  bool f32(float *v) { return fixed(v, sizeof *v); }
  bool f64(double *v) { return fixed(v, sizeof *v); }
  bool flag(bool *v);
  /** Well-formed UTF-8 of at most max bytes. */
  bool str(std::string *s, size_t max = SIZE_MAX) {
    return text(s, true, max);
  }
  /** Text of a value: well-formed UTF-8 from a worker, any bytes from the
      host. */
  bool lstr(std::string *s) { return text(s, m_from == Sender::kWorker, SIZE_MAX); }
  /** Any bytes. */
  bool bytes(std::string *s) { return text(s, false, SIZE_MAX); }
  /** An element count: at most what the rest of the body can hold
      (min_size bytes each) and at most max. */
  bool count(uint32_t *n, size_t min_size, uint32_t max = UINT32_MAX);
  bool value(Value *v);
  /** Fails unless every byte was consumed. */
  bool finish();

  bool fail(std::string what);
  bool ok() const { return m_error.empty(); }
  const std::string &error() const { return m_error; }
  size_t offset() const {
    return m_origin + static_cast<size_t>(m_pos - m_begin);
  }
  size_t remaining() const { return static_cast<size_t>(m_end - m_pos); }

 private:
  bool fixed(void *out, size_t n);
  bool text(std::string *s, bool utf8, size_t max);
  bool temporal(Tag tag, Temporal *t);
  bool vector(std::vector<float> *v);

  const unsigned char *m_begin, *m_pos, *m_end;
  Sender m_from;
  size_t m_origin;
  std::string m_error;
};

/** Well-formed UTF-8 (§7.1). */
bool is_valid_utf8(std::string_view s);
/** Replaces each maximal subpart of an ill-formed UTF-8 sequence by U+FFFD
    (§7.1): for the text the host composes or takes from the server for a
    str field (names, error and warning messages). Never for code or names
    of libraries: those cross as they are or not at all. */
void repair_utf8(std::string *text);
/** -?[0-9]+(\.[0-9]+)?, 1 to 100 bytes. */
bool is_valid_decimal(std::string_view s);
/** The domain of a DATE, TIME or DATETIME. */
bool is_valid_temporal(Tag tag, const Temporal &t);
/** The domain of a TIMESTAMP. */
bool is_valid_timestamp(int64_t microseconds);
/** A language identifier or worker class: 1 to 32 characters of [a-z0-9_]. */
bool is_valid_identifier(std::string_view s);
/** Empty, or 5 characters of [0-9A-Z]. */
bool is_valid_sqlstate(std::string_view s);
/** A LIBRARY schema or name: 1 to 256 bytes without U+0000. */
bool is_valid_library_name(std::string_view s);
/** True unless one of the values is a float that is not finite. */
bool all_finite(std::span<const Value> values);

// ---------------------------------------------------------------- structures

/** The specification's Type (§7.4). */
struct ParamType {
  Tag tag = Tag::kNull;
  bool is_unsigned = false;
  uint32_t length = 0;
  uint32_t precision = 0;
  uint32_t scale = 0;
  std::string type_name;
  bool operator==(const ParamType &) const = default;
};

struct Param {
  std::string name;
  ParamMode mode = ParamMode::kIn;
  ParamType type;
  bool operator==(const Param &) const = default;
};

struct Column {
  Column() = default;
  Column(std::string name_, std::string table_, ParamType type_)
      : name(std::move(name_)), table(std::move(table_)), type(std::move(type_)) {}

  std::string name;
  std::string table;
  ParamType type;
  std::string org_name;
  std::string schema;
  std::string org_table;
  std::string charset;
  std::string collation;
  uint32_t flags = 0;
  bool operator==(const Column &) const = default;
};

/** The specification's Frame. */
struct StackFrame {
  std::string function;
  std::string file;
  uint32_t line = 0;
  uint32_t column = 0;
  uint32_t end_column = 0;
  std::string text;
  bool operator==(const StackFrame &) const = default;
};

struct Error {
  /** A language of the worker's READY, "", or a marker (wire::kLanguageType,
      kLanguageTimeZone, kLanguageOption). */
  std::string language;
  std::string message;
  std::vector<StackFrame> stack;
  uint32_t sql_errno = 0;
  std::string sqlstate;
  /** The error_id of the SQL_RESULT ERROR this error hands back, or 0
      (§10.8). */
  uint64_t sql_error_id = 0;
  bool operator==(const Error &) const = default;
};

struct Warning {
  uint32_t level = 0;
  uint32_t code = 0;
  std::string message;
  bool operator==(const Warning &) const = default;
};

// ---------------------------------------------------------------- messages

/** READY (§9.1). A failed READY (status INTERNAL) may end after info: see
    encode_failed_ready() and decode_ready_head(). */
struct ReadyMsg {
  uint32_t version = wire::kVersion;
  std::string info;
  uint64_t features = 0;
  std::string worker_class;
  std::vector<std::string> languages;
  std::string runtime;
  bool operator==(const ReadyMsg &) const = default;
};

/** A library of a routine's USING clause. */
struct Import {
  std::string schema;
  std::string name;
  /** "": the library's own name. */
  std::string alias;
  bool operator==(const Import &) const = default;
};

/** PREPARE (§9.2). */
struct PrepareMsg {
  uint64_t routine_id = 0;
  std::string language;
  std::string name;
  std::string entry;
  std::string source;
  std::vector<Param> params;
  std::optional<ParamType> returns;
  std::vector<Import> imports;
  std::string schema;
  bool check = false;
  uint64_t library_epoch = 0;
  bool operator==(const PrepareMsg &) const = default;
};

/** PARSE_LIBRARY (§9.9). */
struct ParseLibraryMsg {
  std::string language;
  std::string name;
  std::string body;
  bool operator==(const ParseLibraryMsg &) const = default;
};

/** LIBRARY (§9.10). */
struct LibraryMsg {
  std::string schema;
  std::string name;
  /** The version of the body the worker holds, or 0. */
  uint64_t known_version = 0;
  bool operator==(const LibraryMsg &) const = default;
};

/** LIBRARY_RESULT (§9.10). */
struct LibraryResultMsg {
  wire::LibraryStatus status = wire::LibraryStatus::kRefused;
  std::string language;
  std::string body;
  /** The host's token for this (language, body); 0 with kRefused. */
  uint64_t version = 0;
  std::string message;
  bool operator==(const LibraryResultMsg &) const = default;
};

/** CALL (§9.3). */
struct CallMsg {
  uint64_t routine_id = 0;
  uint64_t library_epoch = 0;
  std::vector<Value> args;
  bool operator==(const CallMsg &) const = default;
};

/** RESULT (§9.4). */
struct ResultMsg {
  std::optional<Value> ret;
  /** (index of the Param, value) for the OUT and INOUT parameters. */
  std::vector<std::pair<uint32_t, Value>> out_params;
  std::string console_out;
  std::string console_err;
  bool console_cleared = false;
  std::vector<std::string> findings;
  std::optional<Error> error;
  bool operator==(const ResultMsg &) const = default;
  /** Back to a RESULT with nothing in it, without building a new one. */
  void clear() {
    ret.reset();
    out_params.clear();
    console_out.clear();
    console_err.clear();
    console_cleared = false;
    findings.clear();
    error.reset();
  }
};

/** SQL (§9.5, §10.2). */
struct SqlMsg {
  wire::SqlAction action = wire::SqlAction::kExecute;
  uint64_t stmt_id = 0;
  std::string statement;
  std::vector<Value> params;
  uint8_t flags = 0;
  uint32_t max_rows = 0;
  uint32_t row_width = 0;
  bool operator==(const SqlMsg &) const = default;
};

/** SQL_RESULT (§9.5, §10.3). */
struct SqlResultMsg {
  SqlResultKind kind = SqlResultKind::kOk;
  // kError
  uint32_t sql_errno = 0;
  std::string sqlstate;
  std::string message;
  uint32_t failed_row = wire::kNoRow;
  uint64_t error_id = 0;
  // kOk
  uint64_t affected_rows = 0;
  uint64_t insert_id = 0;
  // kRows
  /** The set's columns in its first batch, empty in every later one. */
  std::vector<Column> columns;
  /** The set's column count in a later batch (columns is empty); else 0. */
  uint32_t ncolumns = 0;
  uint64_t total_rows = wire::kUnknownTotalRows;
  /** Row by row, rows x column count values. */
  std::vector<Value> rows;
  bool more = false;
  // kOk and kRows
  bool more_results = false;
  std::vector<Warning> warnings;
  uint64_t exec_us = 0;
  // kOk in reply to PREPARE
  uint32_t param_count = 0;
  bool operator==(const SqlResultMsg &) const = default;
};

/** DROP (§9.6). */
struct DropMsg {
  uint64_t routine_id = 0;
  bool operator==(const DropMsg &) const = default;
};

struct Setting {
  std::string name;
  std::string value;
  bool operator==(const Setting &) const = default;
};

/** OPTIONS (§9.8). settings are sorted ascending by the bytes of name. */
struct OptionsMsg {
  std::string time_zone = "+00:00";
  std::vector<Setting> settings;
  bool operator==(const OptionsMsg &) const = default;
};

/** The domains of OPTIONS: the zone's and the names' and values' lengths,
    the names' characters, their order and the count. */
bool is_valid(const OptionsMsg &m, std::string *error);

/** Each encode() replaces *out with the message body. */
void encode(const ReadyMsg &m, std::string *out);
/** The body of a failed READY: version and info only. */
void encode_failed_ready(uint32_t version, std::string_view info,
                         std::string *out);
void encode(const PrepareMsg &m, std::string *out);
void encode(const CallMsg &m, std::string *out);
void encode(const ResultMsg &m, std::string *out);
void encode(const SqlMsg &m, std::string *out);
void encode(const SqlResultMsg &m, std::string *out);
/** A ROWS SQL_RESULT written in parts: the head (kind, columns, total rows,
    and a row count to patch at the returned position), the rows' values
    (Writer::*_value), then the tail. encode(SqlResultMsg) uses the same
    parts. A later batch of a set passes no columns. */
size_t put_rows_head(Writer &w, const std::vector<Column> &columns,
                     uint64_t total_rows);
void put_rows_tail(Writer &w, bool more, bool more_results,
                   const std::vector<Warning> &warnings, uint64_t exec_us);
void encode(const DropMsg &m, std::string *out);
void encode(const OptionsMsg &m, std::string *out);
void encode(const ParseLibraryMsg &m, std::string *out);
void encode(const LibraryMsg &m, std::string *out);
void encode(const LibraryResultMsg &m, std::string *out);

/** Messages of a worker. */
bool decode(std::string_view body, ReadyMsg *m, std::string *error);
/** The first field of a READY, without looking at the rest. */
bool decode_ready_version(std::string_view body, uint32_t *version,
                          std::string *error);
/** The two fields of a failed READY; what follows them is ignored. */
bool decode_ready_head(std::string_view body, uint32_t *version,
                       std::string *info, std::string *error);
bool decode(std::string_view body, ResultMsg *m, std::string *error);
bool decode(std::string_view body, SqlMsg *m, std::string *error);
/** An SQL message after its action and statement id (the first
    wire::kSqlHeadSize bytes, read by the caller into m->action and
    m->stmt_id): rest is the body from that offset. */
bool decode_sql_rest(std::string_view rest, SqlMsg *m, std::string *error);
bool decode(std::string_view body, LibraryMsg *m, std::string *error);
/** Messages of the host. A later batch of a result set carries no columns:
    set_columns is the set's column count then. */
bool decode(std::string_view body, PrepareMsg *m, std::string *error);
bool decode(std::string_view body, CallMsg *m, std::string *error);
bool decode(std::string_view body, SqlResultMsg *m, std::string *error,
            uint32_t set_columns = 0);
bool decode(std::string_view body, DropMsg *m, std::string *error);
bool decode(std::string_view body, OptionsMsg *m, std::string *error);
bool decode(std::string_view body, ParseLibraryMsg *m, std::string *error);
bool decode(std::string_view body, LibraryResultMsg *m, std::string *error);

// ---------------------------------------------------------------- chunks

/** Reassembles a message that may arrive in several chunks (§5.2). The
    caller copies each MessageHeader out of the region first and passes the
    copy; add() copies the chunk's bytes once, so the arena may change
    afterwards (or, for a message of one chunk, leaves them for a decoder
    that reads each byte once). Every chunk is checked against the arena size, the message
    size limit and the first chunk (its op, status, depth, call_seq,
    total_len and TAINTED flag). What only the receiver knows (the ops
    allowed, the expected depth and call_seq, the limit of this op) it
    checks itself on the first chunk, before add(). */
class Assembler {
 public:
  enum class Step { kNeedMore, kDone, kError };

  Assembler(uint32_t arena_size, uint64_t max_message)
      : m_arena_size(arena_size), m_max_message(max_message) {}

  /** keep false: the chunk is checked and counted, not copied (a message of
      one chunk that the caller decodes where it is). */
  Step add(const wire::MessageHeader &h, const void *chunk, bool keep = true);
  /** After the first chunk: the body keeps only its first keep bytes; the
      chunks that follow are checked and counted, not stored (§5.3). */
  void discard_beyond(size_t keep);
  bool discarding() const { return m_discard; }
  /** The first chunk's header (total_len is the body size). */
  const wire::MessageHeader &header() const { return m_first; }
  const std::string &body() const { return m_body; }
  /** Bytes of the message received so far. */
  uint64_t received() const { return m_received; }
  /** The body, moved out (the Assembler is done with it). */
  std::string release_body() { return std::move(m_body); }
  const std::string &error() const { return m_error; }
  void reset();

 private:
  Step fail(std::string what);

  uint32_t m_arena_size;
  uint64_t m_max_message;
  bool m_started = false;
  bool m_discard = false;
  uint64_t m_received = 0;
  wire::MessageHeader m_first{};
  std::string m_body;
  std::string m_error;
};

/** Checks the header of a message a worker posted (already copied out),
    as far as the header alone tells (§13): an assigned op, status 0 unless
    the op is RESULT (0 to 4) or READY (0 or 4), no flag but MORE and, in a
    RESULT, TAINTED, reserved 0, len within the arena, MORE only with a full arena, total_len within a
    worker's limit, and every field of a CONTINUE but depth and call_seq.
    Whether the op is allowed, depth, call_seq and the limit of the op are
    the caller's to check. */
bool check_worker_header(const wire::MessageHeader &h, uint32_t arena_size,
                         std::string *error);

}  // namespace mle

#endif  // MLE_CODEC_H
