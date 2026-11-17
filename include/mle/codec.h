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

  Encoding and decoding of MLE wire protocol v1 message bodies (protocol.h),
  and reassembly of chunked messages.

  Decoding treats its input as hostile: every length and count is checked
  against the bytes that are left, every value is checked for its domain
  (UTF-8, temporal ranges, DECIMAL syntax, VECTOR size and padding), and a
  body must be consumed exactly. A decoder returns false and sets an error
  text on the first problem; it never reads outside its input.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mle/protocol.h"
#include "mle/value.h"

namespace mle {

using wire::ParamMode;
using wire::SqlResultKind;

/** Appends wire encodings to a byte string, or writes them into a fixed
    buffer (the mailbox arena: exp note 44, step 2). Offsets (for VECTOR
    alignment) are relative to the start of the string or buffer, which is
    the message start. In a fixed buffer, a write that does not fit sets
    overflow() and writes nothing; set_position() rolls back. */
class Writer {
 public:
  explicit Writer(std::string *out) : m_out(out) {}
  Writer(char *buffer, size_t capacity) : m_buf(buffer), m_cap(capacity) {}

  void u8(uint8_t v) { raw(&v, sizeof v); }
  void u16(uint16_t v) { raw(&v, sizeof v); }
  void u32(uint32_t v) { raw(&v, sizeof v); }
  void u64(uint64_t v) { raw(&v, sizeof v); }
  void f32(float v) { raw(&v, sizeof v); }
  void f64(double v) { raw(&v, sizeof v); }
  void str(std::string_view s);
  void value(const Value &v);
  /** Values without an mle::Value, encoded exactly as value() encodes them. */
  void null_value() { u8(static_cast<uint8_t>(Tag::kNull)); }
  void i64_value(int64_t v);
  void u64_value(uint64_t v);
  void f64_value(double v);
  /** kDecimal, kString, kBytes or kJson. */
  void text_value(Tag tag, std::string_view s);
  /** kDate, kTime, kDatetime or kTimestamp. */
  void temporal_value(Tag tag, const Temporal &t);
  /** dims floats from data (any alignment), padded to kVectorAlignment. */
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
  Reader(const void *data, size_t len)
      : m_begin(static_cast<const unsigned char *>(data)),
        m_pos(m_begin),
        m_end(m_begin + len) {}
  explicit Reader(std::string_view s) : Reader(s.data(), s.size()) {}

  bool u8(uint8_t *v) { return fixed(v, sizeof *v); }
  bool u16(uint16_t *v) { return fixed(v, sizeof *v); }
  bool u32(uint32_t *v) { return fixed(v, sizeof *v); }
  bool u64(uint64_t *v) { return fixed(v, sizeof *v); }
  bool f32(float *v) { return fixed(v, sizeof *v); }
  bool f64(double *v) { return fixed(v, sizeof *v); }
  bool flag(bool *v);
  /** UTF-8 string. */
  bool str(std::string *s) { return text(s, true); }
  /** Byte string (any bytes). */
  bool bytes(std::string *s) { return text(s, false); }
  /** A u32 element count that can be satisfied by the remaining bytes when
      each element takes at least min_size bytes. Protects against huge
      allocations from a hostile count. */
  /** An element count: at most what the rest of the message can hold
      (min_size bytes each) and at most max (decoded elements can be much
      larger than their wire form). */
  bool count(uint32_t *n, size_t min_size, uint32_t max = UINT32_MAX);
  bool value(Value *v);
  /** Fails unless every byte was consumed. */
  bool finish();

  bool fail(std::string what);
  bool ok() const { return m_error.empty(); }
  const std::string &error() const { return m_error; }
  size_t offset() const { return static_cast<size_t>(m_pos - m_begin); }
  size_t remaining() const { return static_cast<size_t>(m_end - m_pos); }

 private:
  bool fixed(void *out, size_t n);
  bool text(std::string *s, bool utf8);
  bool temporal(Tag tag, Temporal *t);
  bool vector(std::vector<float> *v);

  const unsigned char *m_begin, *m_pos, *m_end;
  std::string m_error;
};

/** Well-formed UTF-8 (no overlong forms, no surrogates, <= U+10FFFF). */
bool is_valid_utf8(std::string_view s);
/** [-]digits[.digits], at most 100 characters. */
bool is_valid_decimal(std::string_view s);
/** Domain check of a temporal value for its tag. */
bool is_valid_temporal(Tag tag, const Temporal &t);

// ---------------------------------------------------------------- messages

struct ParamType {
  Tag tag = Tag::kNull;
  bool is_unsigned = false;
  /** Character length, byte length or VECTOR dimensions. */
  uint32_t length = 0;
  uint32_t precision = 0;
  uint32_t scale = 0;
  /** INT, VARCHAR, SET, YEAR, BIT ... (protocol.h). */
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

  /** The label (alias) and the table's alias. */
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

struct StackFrame {
  std::string function;
  std::string file;
  uint32_t line = 0;
  uint32_t column = 0;
  uint32_t end_column = 0;
  /** The frame's source line (trimmed), for tracebacks; may be empty. */
  std::string text;
  bool operator==(const StackFrame &) const = default;
};

struct Error {
  std::string language;
  std::string message;
  std::vector<StackFrame> stack;
  /** 0 unless the error is a MySQL error that crossed the guest unchanged. */
  uint32_t sql_errno = 0;
  std::string sqlstate;
  bool operator==(const Error &) const = default;
};

struct Warning {
  uint32_t level = 0;
  uint32_t code = 0;
  std::string message;
  bool operator==(const Warning &) const = default;
};

struct ReadyMsg {
  uint32_t version = wire::kVersion;
  std::string worker_class;
  std::vector<std::string> languages;
  std::string runtime;
  std::string info;
  bool operator==(const ReadyMsg &) const = default;
};

/** A library of a routine's USING clause (Phase 6). */
struct Import {
  std::string schema;
  std::string name;
  /** "": the routine uses the library's name. */
  std::string alias;
  bool operator==(const Import &) const = default;
};

struct PrepareMsg {
  uint64_t routine_id = 0;
  std::string language;
  std::string name;
  /** Empty: source is a routine body (protocol.h). */
  std::string entry;
  std::string source;
  std::vector<Param> params;
  std::optional<ParamType> returns;
  std::vector<Import> imports;
  /** The routine's schema: the worker keeps routines by schema too
      (JavaRoutines' cache: db1.f and db2.f with the same text are not one). */
  std::string schema;
  /** CREATE: also check the code beyond its syntax (Python: names never
      defined); findings come back as marked stderr lines (exp note 39). */
  bool check = false;
  bool operator==(const PrepareMsg &) const = default;
};

/** CREATE LIBRARY's syntax check (Phase 6). */
struct ParseLibraryMsg {
  std::string language;
  std::string name;
  std::string body;
  bool operator==(const ParseLibraryMsg &) const = default;
};

/** A library the worker imports (Phase 6). */
struct LibraryMsg {
  std::string schema;
  std::string name;
  uint64_t known_hash = 0;
  bool operator==(const LibraryMsg &) const = default;
};

struct LibraryResultMsg {
  wire::LibraryStatus status = wire::LibraryStatus::kRefused;
  std::string language;
  std::string body;
  uint64_t hash = 0;
  std::string message;
  bool operator==(const LibraryResultMsg &) const = default;
};

/** The hash of a library body in kLibrary/kLibraryResult (FNV-1a 64; 0 is
    reserved for "none"). */
uint64_t library_hash(std::string_view body);

struct CallMsg {
  uint64_t routine_id = 0;
  uint32_t events = 0;
  std::vector<Value> args;
  bool operator==(const CallMsg &) const = default;
};

struct ResultMsg {
  std::optional<Value> ret;
  /** (parameter index, value) for OUT and INOUT parameters. */
  std::vector<std::pair<uint32_t, Value>> out_params;
  std::string console_out;
  std::string console_err;
  /** The routine cleared the console (before console_out/err). */
  bool console_cleared = false;
  std::optional<Error> error;
  bool operator==(const ResultMsg &) const = default;
};

struct SqlMsg {
  wire::SqlAction action = wire::SqlAction::kExecute;
  uint64_t stmt_id = 0;
  std::string statement;
  std::vector<Value> params;
  uint8_t flags = 0;
  uint32_t max_rows = 0;
  bool operator==(const SqlMsg &) const = default;
};

struct SqlResultMsg {
  SqlResultKind kind = SqlResultKind::kOk;
  // kError
  uint32_t sql_errno = 0;
  std::string sqlstate;
  std::string message;
  // kOk
  uint64_t affected_rows = 0;
  uint64_t insert_id = 0;
  // kRows
  std::vector<Column> columns;
  /** All rows of the result set, also those of other chunks; 0: unknown. */
  uint64_t total_rows = 0;
  /** Row-major, rows x columns.size() values. */
  std::vector<Value> rows;
  bool more = false;
  // kOk and kRows
  bool more_results = false;
  std::vector<Warning> warnings;
  uint64_t exec_us = 0;
  // kOk after kPrepare
  uint32_t param_count = 0;
  bool operator==(const SqlResultMsg &) const = default;
};

struct DropMsg {
  uint64_t routine_id = 0;
  bool operator==(const DropMsg &) const = default;
};

struct OptionsMsg {
  wire::IntegerType integer_type = wire::IntegerType::kUnsafeString;
  wire::DecimalType decimal_type = wire::DecimalType::kString;
  std::string time_zone = "+00:00";
  bool operator==(const OptionsMsg &) const = default;
};

/** Each encode() replaces *out with the message body. */
void encode(const ReadyMsg &m, std::string *out);
void encode(const PrepareMsg &m, std::string *out);
void encode(const CallMsg &m, std::string *out);
void encode(const ResultMsg &m, std::string *out);
void encode(const SqlMsg &m, std::string *out);
void encode(const SqlResultMsg &m, std::string *out);
/** A kRows SQL_RESULT written in parts (exp note 44, step 2): the head (kind,
    columns, total rows, a row count to patch at the returned position), the rows' values
    (Writer::*_value), then the tail. encode(SqlResultMsg) uses the same
    parts, so both give the same bytes. */
size_t put_rows_head(Writer &w, const std::vector<Column> &columns,
                     uint64_t total_rows);
void put_rows_tail(Writer &w, bool more, bool more_results,
                   const std::vector<Warning> &warnings, uint64_t exec_us);
void encode(const DropMsg &m, std::string *out);
void encode(const OptionsMsg &m, std::string *out);
void encode(const ParseLibraryMsg &m, std::string *out);
void encode(const LibraryMsg &m, std::string *out);
void encode(const LibraryResultMsg &m, std::string *out);

bool decode(std::string_view body, ReadyMsg *m, std::string *error);
bool decode(std::string_view body, PrepareMsg *m, std::string *error);
bool decode(std::string_view body, CallMsg *m, std::string *error);
bool decode(std::string_view body, ResultMsg *m, std::string *error);
bool decode(std::string_view body, SqlMsg *m, std::string *error);
bool decode(std::string_view body, SqlResultMsg *m, std::string *error);
bool decode(std::string_view body, DropMsg *m, std::string *error);
bool decode(std::string_view body, OptionsMsg *m, std::string *error);
bool decode(std::string_view body, ParseLibraryMsg *m, std::string *error);
bool decode(std::string_view body, LibraryMsg *m, std::string *error);
bool decode(std::string_view body, LibraryResultMsg *m, std::string *error);

// ---------------------------------------------------------------- chunks

/** Reassembles a message that may arrive in several chunks. The caller
    copies each MessageHeader out of shared memory first and passes the
    copy; add() copies the chunk bytes, so the arena may change afterwards.
    All header fields of a chunk are checked against the arena size, the
    message size limit and the first chunk. */
class Assembler {
 public:
  enum class Step { kNeedMore, kDone, kError };

  Assembler(uint32_t arena_size, uint64_t max_message)
      : m_arena_size(arena_size), m_max_message(max_message) {}

  Step add(const wire::MessageHeader &h, const void *chunk);
  /** The first chunk's header (total_len is the body size). */
  const wire::MessageHeader &header() const { return m_first; }
  const std::string &body() const { return m_body; }
  /** The body, moved out (the Assembler is done with it). */
  std::string release_body() { return std::move(m_body); }
  const std::string &error() const { return m_error; }
  void reset();

 private:
  Step fail(std::string what);

  uint32_t m_arena_size;
  uint64_t m_max_message;
  bool m_started = false;
  wire::MessageHeader m_first{};
  std::string m_body;
  std::string m_error;
};

/** Checks a header the worker posted (already copied out): known op and
    flags, a status a worker may send, len within the arena. Depth and
    call_seq are checked by the caller, which knows the expected values. */
bool check_worker_header(const wire::MessageHeader &h, uint32_t arena_size,
                         std::string *error);

}  // namespace mle

#endif  // MLE_CODEC_H
