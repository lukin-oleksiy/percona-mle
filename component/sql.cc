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

#include "sql.h"

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/mysql_thd_attributes_bits.h>
#include <mysql/components/services/bits/stored_program_bits.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_statement_service.h>
#include <mysql/components/services/mysql_thd_attributes.h>
#include <mysqld_error.h>

#include "mle_component.h"
#include "routine.h"

extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_factory);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_execute);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_execute_direct);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_bind);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_result);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_resultset_metadata);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_metadata);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_diagnostics);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_attributes);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_get_string);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_get_integer);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_get_double);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_get_unsigned_integer);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_stmt_get_time);
extern REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_attributes);

namespace mle_component {

using mle::SqlResultMsg;
using mle::Tag;
using mle::Value;
using mle::wire::SqlAction;
using mle::wire::SqlResultKind;

namespace {

/** How a column's values are read (S-3: the getter must match the type;
    the string getter on a non-string column reports NULL). */
enum class Reader {
  kSigned,
  kUnsigned,  // BIGINT UNSIGNED: U64
  kUnsignedSmall,
  kDouble,
  kDecimal,
  kText,
  kBytes,
  kJson,
  kBit,
  kVector,
  kDate,
  kTime,
  kDatetime,
  kTimestamp,
};

mysql_cstring_with_length cstr(const char *s) { return {s, strlen(s)}; }

template <typename T>
bool set_attribute(my_h_statement h, const char *name, const T &value) {
  return mysql_service_mysql_stmt_attributes->set(h, cstr(name), &value);
}

std::string text_info(my_h_field f, const char *key) {
  const char *v = nullptr;
  if (mysql_service_mysql_stmt_resultset_metadata->field_info(f, key, &v) ||
      v == nullptr)
    return "";
  return v;
}

template <typename T>
T number_info(my_h_field f, const char *key) {
  T v{};
  (void)mysql_service_mysql_stmt_resultset_metadata->field_info(f, key, &v);
  return v;
}

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

struct SqlRunner::Statement {
  my_h_statement h = nullptr;
  /** From PREPARE: kept until CLOSE (or the end of the call). */
  bool prepared = false;
  bool to_client = false;
  uint32_t param_count = 0;
  /** The current result set has rows not sent yet. */
  bool rows_open = false;
  /** The next result set is current but not reported yet. */
  bool next_pending = false;
  std::vector<mle::Column> columns;
  std::vector<Reader> readers;
  /** One row read ahead (to know whether more rows follow). */
  std::vector<Value> lookahead;

  ~Statement() {
    if (h != nullptr) (void)mysql_service_mysql_stmt_factory->close(h);
  }
};

void sql_error(SqlResultMsg *result, uint32_t sql_errno,
               const std::string &message) {
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kError;
  result->sql_errno = sql_errno;
  result->sqlstate = "HY000";
  result->message = message;
}

SqlRunner::SqlRunner(MYSQL_THD thd) : m_thd(thd) {}

SqlRunner::~SqlRunner() { m_statements.clear(); }

void SqlRunner::close(uint64_t id) { m_statements.erase(id); }

void SqlRunner::clear_pending_error() {
  if (!m_pending) return;
  uint16_t empty = STATUS_DA_EMPTY;
  (void)mysql_service_mysql_thd_attributes->set(m_thd, "da_status", &empty);
  m_pending = false;
}

void SqlRunner::statement_error(Statement &s, SqlResultMsg *result) {
  uint64_t err_no = 0;
  mysql_cstring_with_length msg{"", 0}, state{"", 0};
  if (mysql_service_mysql_stmt_diagnostics->error_number(s.h, &err_no) ||
      mysql_service_mysql_stmt_diagnostics->error(s.h, &msg) ||
      mysql_service_mysql_stmt_diagnostics->sqlstate(s.h, &state) ||
      err_no == 0) {
    sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
              "the statement failed without diagnostics");
    return;
  }
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kError;
  result->sql_errno = static_cast<uint32_t>(err_no);
  result->sqlstate.assign(state.str, state.length);
  result->message.assign(msg.str, msg.length);
  // The error is in the caller's diagnostics area now (S-3).
  m_pending = true;
  m_pending_errno = result->sql_errno;
  m_pending_sqlstate = result->sqlstate;
}

void SqlRunner::handle(const mle::SqlMsg &request, SqlResultMsg *result) {
  // The worker sent another request: it went on after an error it got, so
  // that error must not stay in the caller's diagnostics area.
  clear_pending_error();
  switch (request.action) {
    case SqlAction::kExecute:
      return execute_text(request, result);
    case SqlAction::kPrepare:
      return prepare(request, result);
    case SqlAction::kExecutePrepared:
      return execute_prepared(request, result);
    case SqlAction::kFetch:
      return fetch(request, result);
    case SqlAction::kNextResult:
      return next_result(request, result);
    case SqlAction::kClose:
      close(request.stmt_id);
      *result = SqlResultMsg();
      result->kind = SqlResultKind::kOk;
      return;
  }
}

// ------------------------------------------------------------- statements

namespace {

/** A new statement handle with our attributes; nullptr on failure. */
my_h_statement new_statement(bool to_client, size_t capacity) {
  my_h_statement h = nullptr;
  if (mysql_service_mysql_stmt_factory->init(&h)) return nullptr;
  // S-3: the value must be a mysql_cstring_with_length (a char* crashes).
  const mysql_cstring_with_length utf8mb4 = cstr("utf8mb4");
  bool failed = set_attribute(h, "charset_name", utf8mb4) ||
                set_attribute(h, "buffer_capacity", capacity);
  if (to_client) failed = failed || set_attribute(h, "use_thd_protocol", true);
  if (failed) {
    (void)mysql_service_mysql_stmt_factory->close(h);
    return nullptr;
  }
  return h;
}

/** Binds value v to marker index (S-11: all kinds bind). */
bool bind_value(my_h_statement h, uint32_t index, const Value &v) {
  auto bind = [&](bool is_null, uint64_t type, bool is_unsigned,
                  const void *data, unsigned long length) {
    return mysql_service_mysql_stmt_bind->bind_param(
        h, index, is_null, type, is_unsigned, data, length, nullptr, 0);
  };
  switch (v.tag()) {
    case Tag::kNull:
      return bind(true, MYSQL_SP_ARG_TYPE_NULL, false, nullptr, 0);
    case Tag::kI64: {
      const int64_t x = v.as_i64();
      return bind(false, MYSQL_SP_ARG_TYPE_LONGLONG, false, &x, sizeof x);
    }
    case Tag::kU64: {
      const uint64_t x = v.as_u64();
      return bind(false, MYSQL_SP_ARG_TYPE_LONGLONG, true, &x, sizeof x);
    }
    case Tag::kF64:
    case Tag::kF32: {
      const double x = v.tag() == Tag::kF64 ? v.as_f64() : v.as_f32();
      return bind(false, MYSQL_SP_ARG_TYPE_DOUBLE, false, &x, sizeof x);
    }
    case Tag::kDecimal:
      return bind(false, MYSQL_SP_ARG_TYPE_NEWDECIMAL, false,
                  v.as_text().data(), v.as_text().size());
    case Tag::kString:
    case Tag::kJson:
      return bind(false, MYSQL_SP_ARG_TYPE_VARCHAR, false, v.as_text().data(),
                  v.as_text().size());
    case Tag::kBytes:
      return bind(false, MYSQL_SP_ARG_TYPE_BLOB, false, v.as_text().data(),
                  v.as_text().size());
    case Tag::kVector: {
      // A VECTOR binds as its binary floats (S-11).
      const auto &f = v.as_vector();
      return bind(false, MYSQL_SP_ARG_TYPE_BLOB, false, f.data(),
                  f.size() * sizeof(float));
    }
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime:
    case Tag::kTimestamp: {
      const mle::Temporal &t = v.as_temporal();
      if (v.tag() == Tag::kTime && t.negative) {
        // The service drops mle_time::neg (convert_to_mysql_time, server
        // bug): bind a negative TIME as text, the server converts it.
        char b[48];
        snprintf(b, sizeof b, "-%u:%02u:%02u.%06u", t.hour, t.minute, t.second,
                 t.microsecond);
        return bind(false, MYSQL_SP_ARG_TYPE_VARCHAR, false, b, strlen(b));
      }
      mle_time m{};
      m.year = t.year;
      m.month = t.month;
      m.day = t.day;
      m.hour = t.hour;
      m.minute = t.minute;
      m.second = t.second;
      m.second_part = t.microsecond;
      m.neg = t.negative;
      uint64_t type = MYSQL_SP_ARG_TYPE_DATETIME;
      m.time_type = MYSQL_TIMESTAMP_TYPE_DATETIME;
      if (v.tag() == Tag::kDate) {
        type = MYSQL_SP_ARG_TYPE_DATE;
        m.time_type = MYSQL_TIMESTAMP_TYPE_DATE;
      } else if (v.tag() == Tag::kTime) {
        type = MYSQL_SP_ARG_TYPE_TIME;
        m.time_type = MYSQL_TIMESTAMP_TYPE_TIME;
      }
      return bind(false, type, false, &m, sizeof m);
    }
  }
  return true;
}

bool bind_values(my_h_statement h, const std::vector<Value> &values) {
  for (uint32_t i = 0; i < values.size(); i++)
    if (bind_value(h, i, values[i])) return true;
  return false;
}

}  // namespace

bool SqlRunner::refused_here(SqlResultMsg *result) {
  // The server refuses statements in stored functions and triggers, but its
  // check for direct statements misses a function called by a trigger
  // (in_sub_stmt compared with ==, not as a mask); the prepared path checks
  // it right. Ask that path once per call (and again while it refuses, so
  // that each refusal is in the diagnostics area).
  if (m_context_checked && !m_refused) return false;
  m_context_checked = true;
  Statement probe;
  probe.h = new_statement(false, 500);
  if (probe.h == nullptr) return false;
  const mysql_cstring_with_length text = cstr("DO 0");
  m_refused = mysql_service_mysql_stmt_execute->prepare(text, probe.h);
  if (m_refused) statement_error(probe, result);
  return m_refused;
}

void SqlRunner::execute_text(const mle::SqlMsg &request,
                             SqlResultMsg *result) {
  if (m_statements.count(request.stmt_id) != 0)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "the statement id is in use");
  if (m_statements.size() >= sql_statements_max())
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "too many open statements (mle.sql_statements_max)");
  const bool to_client =
      (request.flags & mle::wire::kPassResultToClient) != 0;
  const bool with_params = !request.params.empty();
  auto s = std::make_unique<Statement>();
  // S-11: a direct statement buffers its whole result (buffer_capacity
  // bytes); a prepared one uses a cursor, buffer_capacity per fetched row.
  s->h = new_statement(to_client, with_params ? sql_row_max() : sql_result_max());
  if (s->h == nullptr)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "cannot create a statement");
  s->to_client = to_client;
  const mysql_cstring_with_length text{request.statement.data(),
                                       request.statement.size()};
  const uint64_t t0 = now_us();
  bool failed;
  if (with_params) {
    failed = mysql_service_mysql_stmt_execute->prepare(text, s->h) ||
             bind_values(s->h, request.params) ||
             mysql_service_mysql_stmt_execute->execute(s->h);
  } else {
    if (refused_here(result)) return;
    failed = mysql_service_mysql_stmt_execute_direct->execute(text, s->h);
  }
  if (failed) return statement_error(*s, result);
  Statement &st = *s;
  m_statements[request.stmt_id] = std::move(s);
  reply(request.stmt_id, st,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(),
        now_us() - t0, result);
}

void SqlRunner::prepare(const mle::SqlMsg &request, SqlResultMsg *result) {
  if (m_statements.count(request.stmt_id) != 0)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "the statement id is in use");
  if (m_statements.size() >= sql_statements_max())
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "too many open statements (mle.sql_statements_max)");
  const bool to_client =
      (request.flags & mle::wire::kPassResultToClient) != 0;
  auto s = std::make_unique<Statement>();
  s->h = new_statement(to_client, sql_row_max());
  if (s->h == nullptr)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "cannot create a statement");
  s->prepared = true;
  s->to_client = to_client;
  const mysql_cstring_with_length text{request.statement.data(),
                                       request.statement.size()};
  if (mysql_service_mysql_stmt_execute->prepare(text, s->h))
    return statement_error(*s, result);
  (void)mysql_service_mysql_stmt_metadata->param_count(s->h, &s->param_count);
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kOk;
  result->param_count = s->param_count;
  m_statements[request.stmt_id] = std::move(s);
}

void SqlRunner::execute_prepared(const mle::SqlMsg &request,
                                 SqlResultMsg *result) {
  const auto it = m_statements.find(request.stmt_id);
  if (it == m_statements.end() || !it->second->prepared)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "no prepared statement with this id");
  Statement &s = *it->second;
  if (request.params.size() > s.param_count)
    return sql_error(result, ER_WRONG_ARGUMENTS,
                     "more values than parameter markers");
  s.rows_open = false;
  s.next_pending = false;
  s.lookahead.clear();
  const uint64_t t0 = now_us();
  // Partial binding: the markers not given keep their values (S-11).
  if (bind_values(s.h, request.params) ||
      mysql_service_mysql_stmt_execute->execute(s.h))
    return statement_error(s, result);
  reply(request.stmt_id, s,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(),
        now_us() - t0, result);
}

namespace {

/** The statement's warnings into result. */
void read_warnings(my_h_statement h, SqlResultMsg *result) {
  uint32_t n = 0;
  (void)mysql_service_mysql_stmt_diagnostics->num_warnings(h, &n);
  for (uint32_t i = 0; i < n; i++) {
    my_h_warning w = nullptr;
    if (mysql_service_mysql_stmt_diagnostics->get_warning(h, i, &w)) break;
    mle::Warning warning;
    mysql_cstring_with_length msg{"", 0};
    (void)mysql_service_mysql_stmt_diagnostics->warning_level(w, &warning.level);
    (void)mysql_service_mysql_stmt_diagnostics->warning_code(w, &warning.code);
    (void)mysql_service_mysql_stmt_diagnostics->warning_message(w, &msg);
    warning.message.assign(msg.str, msg.length);
    result->warnings.push_back(std::move(warning));
  }
}

}  // namespace

void SqlRunner::fetch(const mle::SqlMsg &request, SqlResultMsg *result) {
  const auto it = m_statements.find(request.stmt_id);
  if (it == m_statements.end() || !it->second->rows_open)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "no rows to fetch for this statement id");
  Statement &s = *it->second;
  const uint32_t max_rows =
      request.max_rows != 0 ? request.max_rows : sql_chunk_rows();
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kRows;
  result->columns = s.columns;
  if (read_rows(s, max_rows, result)) return statement_error(s, result);
  if (!s.rows_open) {
    // Drained: its warnings; is there another result set?
    read_warnings(s.h, result);
    bool more = false;
    if (!mysql_service_mysql_stmt_result->next_result(s.h, &more) && more)
      s.next_pending = true;
    result->more_results = s.next_pending;
    if (!s.prepared && !s.next_pending) close(request.stmt_id);
  }
}

void SqlRunner::next_result(const mle::SqlMsg &request, SqlResultMsg *result) {
  const auto it = m_statements.find(request.stmt_id);
  if (it == m_statements.end())
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "no statement with this id");
  Statement &s = *it->second;
  if (!s.next_pending) {
    // Rows of the current set may be left: skip them.
    bool more = false;
    if (s.rows_open) {
      my_h_row row = nullptr;
      while (!mysql_service_mysql_stmt_result->fetch(s.h, &row) && row != nullptr) {
      }
      s.rows_open = false;
      s.lookahead.clear();
    }
    if (mysql_service_mysql_stmt_result->next_result(s.h, &more) || !more) {
      *result = SqlResultMsg();
      result->kind = SqlResultKind::kOk;
      if (!s.prepared) close(request.stmt_id);
      return;
    }
  }
  s.next_pending = false;
  reply(request.stmt_id, s,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(), 0,
        result);
}

// ------------------------------------------------------------- results

void SqlRunner::reply(uint64_t id, Statement &s, uint32_t max_rows,
                      uint64_t exec_us, SqlResultMsg *result) {
  *result = SqlResultMsg();
  result->exec_us = exec_us;
  uint32_t fields = 0;
  (void)mysql_service_mysql_stmt_resultset_metadata->field_count(s.h, &fields);
  if (s.to_client || fields == 0) {
    // No rows for the worker: an OK (the client got any rows).
    result->kind = SqlResultKind::kOk;
    (void)mysql_service_mysql_stmt_diagnostics->affected_rows(
        s.h, &result->affected_rows);
    (void)mysql_service_mysql_stmt_diagnostics->insert_id(s.h,
                                                          &result->insert_id);
    read_warnings(s.h, result);
    bool more = false;
    if (!s.to_client &&
        !mysql_service_mysql_stmt_result->next_result(s.h, &more) && more)
      s.next_pending = true;
    result->more_results = s.next_pending;
    if (!s.prepared && !s.next_pending) close(id);
    return;
  }

  // A result set: its columns, then the first rows.
  s.columns.clear();
  s.readers.clear();
  s.lookahead.clear();
  for (uint32_t i = 0; i < fields; i++) {
    my_h_field f = nullptr;
    (void)mysql_service_mysql_stmt_resultset_metadata->fetch_field(s.h, i, &f);
    mle::Column c;
    c.name = text_info(f, "col_name");
    c.org_name = text_info(f, "org_col_name");
    c.schema = text_info(f, "db_name");
    c.table = text_info(f, "table_name");
    c.org_table = text_info(f, "org_table_name");
    c.charset = text_info(f, "charset_name");
    c.collation = text_info(f, "collation_name");
    c.flags = number_info<uint>(f, "flags");
    const uint64_t type = number_info<uint64_t>(f, "type");
    const bool is_unsigned = number_info<bool>(f, "is_unsigned");
    const bool binary = c.charset == "binary";
    c.type.is_unsigned = is_unsigned;
    c.type.length = static_cast<uint32_t>(number_info<unsigned long>(f, "length"));
    c.type.scale = number_info<uint>(f, "decimals");
    c.type.type_name = sql_type_name(type);
    Reader r;
    switch (type) {
      case MYSQL_SP_ARG_TYPE_TINY:
      case MYSQL_SP_ARG_TYPE_SHORT:
      case MYSQL_SP_ARG_TYPE_INT24:
      case MYSQL_SP_ARG_TYPE_LONG:
      case MYSQL_SP_ARG_TYPE_YEAR:
      case MYSQL_SP_ARG_TYPE_BOOL:
        r = is_unsigned ? Reader::kUnsignedSmall : Reader::kSigned;
        c.type.tag = Tag::kI64;
        break;
      case MYSQL_SP_ARG_TYPE_LONGLONG:
        r = is_unsigned ? Reader::kUnsigned : Reader::kSigned;
        c.type.tag = is_unsigned ? Tag::kU64 : Tag::kI64;
        break;
      case MYSQL_SP_ARG_TYPE_BIT:
        r = Reader::kBit;
        c.type.tag = Tag::kU64;
        break;
      case MYSQL_SP_ARG_TYPE_FLOAT:
        r = Reader::kDouble;
        c.type.tag = Tag::kF32;
        break;
      case MYSQL_SP_ARG_TYPE_DOUBLE:
        r = Reader::kDouble;
        c.type.tag = Tag::kF64;
        break;
      case MYSQL_SP_ARG_TYPE_DECIMAL:
      case MYSQL_SP_ARG_TYPE_NEWDECIMAL:
        r = Reader::kDecimal;
        c.type.tag = Tag::kDecimal;
        break;
      case MYSQL_SP_ARG_TYPE_DATE:
      case MYSQL_SP_ARG_TYPE_NEWDATE:
        r = Reader::kDate;
        c.type.tag = Tag::kDate;
        break;
      case MYSQL_SP_ARG_TYPE_TIME:
      case MYSQL_SP_ARG_TYPE_TIME2:
        r = Reader::kTime;
        c.type.tag = Tag::kTime;
        break;
      case MYSQL_SP_ARG_TYPE_DATETIME:
      case MYSQL_SP_ARG_TYPE_DATETIME2:
        r = Reader::kDatetime;
        c.type.tag = Tag::kDatetime;
        break;
      case MYSQL_SP_ARG_TYPE_TIMESTAMP:
      case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
        r = Reader::kTimestamp;
        c.type.tag = Tag::kTimestamp;
        break;
      case MYSQL_SP_ARG_TYPE_JSON:
        r = Reader::kJson;
        c.type.tag = Tag::kJson;
        break;
      case MYSQL_SP_ARG_TYPE_VECTOR:
        r = Reader::kVector;
        c.type.tag = Tag::kVector;
        c.type.length /= 4;
        break;
      default:  // strings, BLOBs, ENUM, SET, GEOMETRY
        r = binary ? Reader::kBytes : Reader::kText;
        c.type.tag = binary ? Tag::kBytes : Tag::kString;
        break;
    }
    s.columns.push_back(std::move(c));
    s.readers.push_back(r);
  }
  s.rows_open = true;
  result->kind = SqlResultKind::kRows;
  result->columns = s.columns;
  if (read_rows(s, max_rows, result)) return statement_error(s, result);
  if (!s.rows_open) {
    // The result set is complete: its warnings are known.
    read_warnings(s.h, result);
    bool more = false;
    if (!mysql_service_mysql_stmt_result->next_result(s.h, &more) && more)
      s.next_pending = true;
    result->more_results = s.next_pending;
    if (!s.prepared && !s.next_pending) close(id);
  }
}

namespace {

/** One row's values by the column readers; false on a getter failure. */
bool read_row(my_h_row row, const std::vector<Reader> &readers,
              std::vector<Value> *out, size_t *bytes) {
  for (uint32_t i = 0; i < readers.size(); i++) {
    bool is_null = false;
    bool failed = false;
    Value v;
    switch (readers[i]) {
      case Reader::kSigned: {
        int64_t x = 0;
        failed = mysql_service_mysql_stmt_get_integer->get(row, i, &x, &is_null);
        v = Value::i64(x);
        break;
      }
      case Reader::kUnsigned:
      case Reader::kUnsignedSmall: {
        uint64_t x = 0;
        failed = mysql_service_mysql_stmt_get_unsigned_integer->get(row, i, &x,
                                                                    &is_null);
        v = readers[i] == Reader::kUnsigned ? Value::u64(x)
                                            : Value::i64(static_cast<int64_t>(x));
        break;
      }
      case Reader::kDouble: {
        double x = 0;
        failed = mysql_service_mysql_stmt_get_double->get(row, i, &x, &is_null);
        v = Value::f64(x);
        break;
      }
      case Reader::kDate:
      case Reader::kTime:
      case Reader::kDatetime:
      case Reader::kTimestamp: {
        mle_time t{};
        failed = mysql_service_mysql_stmt_get_time->get(row, i, &t, &is_null);
        mle::Temporal tm;
        tm.year = static_cast<uint16_t>(t.year);
        tm.month = static_cast<uint8_t>(t.month);
        tm.day = static_cast<uint8_t>(t.day);
        tm.hour = t.hour;
        tm.minute = static_cast<uint8_t>(t.minute);
        tm.second = static_cast<uint8_t>(t.second);
        tm.microsecond = static_cast<uint32_t>(t.second_part);
        tm.negative = t.neg;
        const Tag tag = readers[i] == Reader::kDate   ? Tag::kDate
                        : readers[i] == Reader::kTime ? Tag::kTime
                        : readers[i] == Reader::kDatetime ? Tag::kDatetime
                                                          : Tag::kTimestamp;
        v = Value::temporal(tag, tm);
        break;
      }
      default: {
        mysql_cstring_with_length s{nullptr, 0};
        failed = mysql_service_mysql_stmt_get_string->get(row, i, &s, &is_null);
        if (failed || is_null) break;
        *bytes += s.length;
        if (readers[i] == Reader::kVector) {
          std::vector<float> f(s.length / sizeof(float));
          memcpy(f.data(), s.str, f.size() * sizeof(float));
          v = Value::vector(std::move(f));
          break;
        }
        std::string text(s.str, s.length);
        switch (readers[i]) {
          case Reader::kDecimal:
            v = Value::decimal(std::move(text));
            break;
          case Reader::kJson:
            v = Value::json(std::move(text));
            break;
          case Reader::kBytes:
            v = Value::bytes(std::move(text));
            break;
          case Reader::kBit: {
            uint64_t x = 0;  // big-endian bytes
            for (unsigned char c : text) x = (x << 8) | c;
            v = Value::u64(x);
            break;
          }
          default:
            v = Value::string(std::move(text));
        }
      }
    }
    if (failed) return false;
    out->push_back(is_null ? Value::null() : std::move(v));
  }
  return true;
}

}  // namespace

bool SqlRunner::read_rows(Statement &s, uint32_t max_rows,
                          SqlResultMsg *result) {
  // A chunk ends after max_rows rows or about one mailbox arena of data:
  // larger messages are split and reassembled, and rows of VECTORs or BLOBs
  // would otherwise be held several times in fresh memory (Phase 7 bench).
  constexpr size_t kChunkBytes = mle::wire::kDefaultArenaSize * 3 / 4;
  size_t bytes = 0;
  uint32_t rows = 0;
  if (!s.lookahead.empty()) {
    for (Value &v : s.lookahead) result->rows.push_back(std::move(v));
    s.lookahead.clear();
    rows++;
  }
  while (rows < max_rows && bytes < kChunkBytes) {
    my_h_row row = nullptr;
    if (mysql_service_mysql_stmt_result->fetch(s.h, &row)) return true;
    if (row == nullptr) {
      s.rows_open = false;
      return false;
    }
    if (!read_row(row, s.readers, &result->rows, &bytes)) return true;
    rows++;
  }
  // The chunk is full: is there one more row?
  my_h_row row = nullptr;
  if (mysql_service_mysql_stmt_result->fetch(s.h, &row)) return true;
  if (row == nullptr) {
    s.rows_open = false;
    return false;
  }
  if (!read_row(row, s.readers, &s.lookahead, &bytes)) return true;
  result->more = true;
  return false;
}

// ------------------------------------------------------------- errors

void SqlRunner::call_succeeded() { clear_pending_error(); }

bool SqlRunner::propagates(const mle::Error &e) {
  if (m_pending && e.sql_errno == m_pending_errno &&
      e.sqlstate == m_pending_sqlstate)
    return true;
  clear_pending_error();
  return false;
}

bool SqlRunner::raise(const mle::Error &e) {
  clear_pending_error();
  // SIGNAL's rules: a 5-character SQLSTATE not of class '00', an error
  // number 1..65535.
  const std::string &state = e.sqlstate;
  bool valid = state.size() == 5 && state.compare(0, 2, "00") != 0 &&
               e.sql_errno >= 1 && e.sql_errno <= 65535;
  for (char c : state)
    valid = valid && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'));
  if (!valid) return false;
  // The message as a hex literal: no quoting or escaping rules apply (also
  // under NO_BACKSLASH_ESCAPES), any bytes are safe in the statement text.
  static const char kHex[] = "0123456789ABCDEF";
  std::string message;
  for (unsigned char c : e.message) {
    message.push_back(kHex[c >> 4]);
    message.push_back(kHex[c & 15]);
  }
  const std::string text =
      "SIGNAL SQLSTATE '" + state + "' SET MESSAGE_TEXT = " +
      (message.empty() ? std::string("''") : "_utf8mb4 X'" + message + "'") +
      ", MYSQL_ERRNO = " + std::to_string(e.sql_errno);
  Statement s;
  s.h = new_statement(false, 500);
  if (s.h == nullptr) return false;
  if (!mysql_service_mysql_stmt_execute_direct->execute(
          {text.data(), text.size()}, s.h))
    return false;  // SIGNAL did not fail: should not happen
  uint64_t err_no = 0;
  (void)mysql_service_mysql_stmt_diagnostics->error_number(s.h, &err_no);
  if (err_no != static_cast<uint64_t>(e.sql_errno)) {
    // In a function SQL is not allowed, so SIGNAL is not either; or the
    // SIGNAL itself failed (e.g. a message that is not UTF-8 or too long):
    // the caller reports the error as a routine error instead.
    m_pending = true;
    clear_pending_error();
    return false;
  }
  return true;  // the SIGNAL error is in the diagnostics area
}

}  // namespace mle_component
