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
  @file components/mle/include/mle/codec.h

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

/** Appends wire encodings to a byte string. Offsets (for VECTOR alignment)
    are relative to the start of the string, which is the message start. */
class Writer {
 public:
  explicit Writer(std::string *out) : m_out(out) {}

  void u8(uint8_t v) { m_out->push_back(static_cast<char>(v)); }
  void u16(uint16_t v) { raw(&v, sizeof v); }
  void u32(uint32_t v) { raw(&v, sizeof v); }
  void u64(uint64_t v) { raw(&v, sizeof v); }
  void f32(float v) { raw(&v, sizeof v); }
  void f64(double v) { raw(&v, sizeof v); }
  void str(std::string_view s);
  void value(const Value &v);
  void raw(const void *p, size_t n) {
    m_out->append(static_cast<const char *>(p), n);
  }

 private:
  std::string *m_out;
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
  bool count(uint32_t *n, size_t min_size);
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
  std::string name;
  std::string table;
  ParamType type;
  bool operator==(const Column &) const = default;
};

struct StackFrame {
  std::string function;
  std::string file;
  uint32_t line = 0;
  uint32_t column = 0;
  uint32_t end_column = 0;
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

struct PrepareMsg {
  uint64_t routine_id = 0;
  std::string language;
  std::string name;
  /** Empty: source is a routine body (protocol.h). */
  std::string entry;
  std::string source;
  std::vector<Param> params;
  std::optional<ParamType> returns;
  bool operator==(const PrepareMsg &) const = default;
};

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
  std::string statement;
  std::vector<Value> params;
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
  /** Row-major, rows x columns.size() values. */
  std::vector<Value> rows;
  bool more = false;
  // kOk and kRows
  std::vector<Warning> warnings;
  bool operator==(const SqlResultMsg &) const = default;
};

struct FetchMsg {
  uint32_t max_rows = 0;
  bool operator==(const FetchMsg &) const = default;
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
void encode(const FetchMsg &m, std::string *out);
void encode(const DropMsg &m, std::string *out);
void encode(const OptionsMsg &m, std::string *out);

bool decode(std::string_view body, ReadyMsg *m, std::string *error);
bool decode(std::string_view body, PrepareMsg *m, std::string *error);
bool decode(std::string_view body, CallMsg *m, std::string *error);
bool decode(std::string_view body, ResultMsg *m, std::string *error);
bool decode(std::string_view body, SqlMsg *m, std::string *error);
bool decode(std::string_view body, SqlResultMsg *m, std::string *error);
bool decode(std::string_view body, FetchMsg *m, std::string *error);
bool decode(std::string_view body, DropMsg *m, std::string *error);
bool decode(std::string_view body, OptionsMsg *m, std::string *error);

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
