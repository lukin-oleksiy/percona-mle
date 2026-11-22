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

#include <algorithm>
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

using mle::SqlRefusal;
using mle::SqlResultMsg;
using mle::Tag;
using mle::Value;
using mle::wire::SqlAction;
using mle::wire::SqlResultKind;

namespace {

/** How a column's values are read (the getter must match the type: the
    string getter on a column that is not a string reports NULL) and which
    tag they are sent with (§7.3). */
enum class Reader {
  kNull,  // a column without a type: only NULLs
  kSigned,
  kUnsigned,  // BIGINT UNSIGNED: U64
  kUnsignedSmall,
  kFloat,
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

/** A text of the result set's metadata, for a str field: repaired. */
std::string text_info(my_h_field f, const char *key) {
  const char *v = nullptr;
  if (mysql_service_mysql_stmt_resultset_metadata->field_info(f, key, &v) ||
      v == nullptr)
    return "";
  std::string text = v;
  repair_utf8(&text);
  return text;
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

/** Forgets the error in the diagnostics area. */
void clear_diagnostics(MYSQL_THD thd) {
  uint16_t empty = STATUS_DA_EMPTY;
  (void)mysql_service_mysql_thd_attributes->set(thd, "da_status", &empty);
}

/** The statement's warnings, at most as many as one list carries (§12);
    their messages may quote column bytes: repaired. */
void read_warnings(my_h_statement h, std::vector<mle::Warning> *warnings) {
  uint32_t n = 0;
  (void)mysql_service_mysql_stmt_diagnostics->num_warnings(h, &n);
  n = std::min(n, mle::wire::kMaxWarnings);
  for (uint32_t i = 0; i < n; i++) {
    my_h_warning w = nullptr;
    if (mysql_service_mysql_stmt_diagnostics->get_warning(h, i, &w)) break;
    mle::Warning warning;
    mysql_cstring_with_length msg{"", 0};
    (void)mysql_service_mysql_stmt_diagnostics->warning_level(w, &warning.level);
    (void)mysql_service_mysql_stmt_diagnostics->warning_code(w, &warning.code);
    (void)mysql_service_mysql_stmt_diagnostics->warning_message(w, &msg);
    warning.message.assign(msg.str, msg.length);
    repair_utf8(&warning.message);
    warnings->push_back(std::move(warning));
  }
}

}  // namespace

struct SqlRunner::Statement {
  my_h_statement h = nullptr;
  /** Opened by PREPARE: at home in IDLE, kept until CLOSE or the end of the
      request. Else opened by EXECUTE: at home in CLOSED. */
  bool prepared = false;
  /** PASS_RESULT_TO_CLIENT, for the life of the id. */
  bool to_client = false;
  uint32_t param_count = 0;
  /** An id in the runner is IDLE, ROWS or NEXT (§10.1); a direct statement
      is never IDLE. */
  enum class State { kIdle, kRows, kNext } state = State::kIdle;
  /** NEXT: the following result is current in the server already. */
  bool next_pending = false;
  /** NEXT: moving to the following result failed; NEXT_RESULT reports it. */
  bool next_failed = false;
  /** An execution of it produced a result set (EXECUTE_MANY refuses it). */
  bool returns_rows = false;
  /** After a result: makes the next one current, if there is one. A
      failure counts as a next result, which NEXT_RESULT reports. */
  void advance() {
    bool more = false;
    next_failed = mysql_service_mysql_stmt_result->next_result(h, &more);
    if (next_failed || more) next_pending = true;
  }
  /** The current result set. */
  std::vector<mle::Column> columns;
  std::vector<Reader> readers;
  /** What the server said of each column when `columns` was made: compared
      with what it says at the next execution. */
  struct Raw {
    uint64_t type = 0;
    unsigned long length = 0;
    uint decimals = 0;
    uint flags = 0;
    bool is_unsigned = false;
  };
  std::vector<Raw> raw;
  /** A prepared id: `columns` are the columns sent last for the id, and the
      worker keeps them (§10.3). */
  bool worker_has_columns = false;
  /** The current set has exactly those columns: its first batch carries
      none. */
  bool omit_columns = false;
  /** Its columns went to the worker with an earlier batch. */
  bool columns_sent = false;
  /** The session's zone when the set has a TIMESTAMP column: a copy, for
      the set is read in the zone it was made in. */
  TimeZone zone;
  /** A row fetched but not sent yet (the server keeps it until the next
      fetch). */
  my_h_row pending = nullptr;
  /** The server holds the whole result (a direct statement, or a prepared
      one without a cursor): every row handle of the current result set is
      collected when it starts, so its row count is known, and stays valid
      until the statement is closed or run again. */
  bool buffered = true;
  std::vector<my_h_row> all;
  size_t next = 0;
  /** The current result set's row count, or that it is not known. */
  uint64_t total = mle::wire::kUnknownTotalRows;

  /** The current result set's next row (nullptr at its end): a collected
      handle, else the server's fetch; true on a server failure. */
  bool next_row(my_h_row *row) {
    if (next < all.size()) {
      *row = all[next++];
      return false;
    }
    // Collected ones used up: the server's fetch says the end (a buffered
    // result), or gives the cursor's next row.
    return mysql_service_mysql_stmt_result->fetch(h, row);
  }

  /** Whether the result set the server has now, of `fields` columns, has
      exactly the columns `columns` was made from. Nothing is allocated. */
  bool same_columns(uint32_t fields) const {
    if (fields != columns.size() || fields != raw.size()) return false;
    for (uint32_t i = 0; i < fields; i++) {
      my_h_field f = nullptr;
      if (mysql_service_mysql_stmt_resultset_metadata->fetch_field(h, i, &f))
        return false;
      const Raw &was = raw[i];
      if (number_info<uint64_t>(f, "type") != was.type ||
          number_info<unsigned long>(f, "length") != was.length ||
          number_info<uint>(f, "decimals") != was.decimals ||
          number_info<uint>(f, "flags") != was.flags ||
          number_info<bool>(f, "is_unsigned") != was.is_unsigned)
        return false;
      // The texts kept are the server's, unless they had to be repaired:
      // those never compare equal, and the columns are sent again.
      const auto same = [f](const char *key, const std::string &kept) {
        const char *v = nullptr;
        if (mysql_service_mysql_stmt_resultset_metadata->field_info(f, key, &v) ||
            v == nullptr)
          return kept.empty();
        return kept == v;
      };
      const mle::Column &c = columns[i];
      if (!same("col_name", c.name) || !same("org_col_name", c.org_name) ||
          !same("db_name", c.schema) || !same("table_name", c.table) ||
          !same("org_table_name", c.org_table) ||
          !same("charset_name", c.charset) ||
          !same("collation_name", c.collation))
        return false;
    }
    return true;
  }

  /** Forgets every result it has: IDLE. */
  void discard() {
    state = State::kIdle;
    next_pending = false;
    next_failed = false;
    columns_sent = false;
    pending = nullptr;
    all.clear();
    next = 0;
  }

  /** The bytes its buffered results hold in the server, and the open
      statement itself, counted in *budget once it has an id. */
  size_t held = 0;
  SqlBudget *budget = nullptr;

  ~Statement() {
    if (budget != nullptr) {
      budget->results_held -= held;
      budget->statements--;
    }
    if (h != nullptr) (void)mysql_service_mysql_stmt_factory->close(h);
  }
};

/** One batch of rows being made. */
struct SqlRunner::Batch {
  uint32_t max_rows = 1;
  uint64_t exec_us = 0;
  /** The result set's first batch. */
  bool first = false;
  /** The batch carries the set's columns. */
  bool columns = false;
  /** The statement's warnings so far. */
  std::vector<mle::Warning> warnings;
  /** A TIMESTAMP could not be turned into an instant. */
  bool bad_timestamp = false;
};

void sql_error(SqlResultMsg *result, uint32_t sql_errno,
               const std::string &message) {
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kError;
  result->sql_errno = sql_errno;
  result->sqlstate = "HY000";
  result->message = message;
}

void refusal_error(SqlRefusal why, const char *no_sql, SqlResultMsg *error) {
  error->sqlstate = "HY000";
  switch (why) {
    case SqlRefusal::kInterrupted:
      error->sql_errno = ER_QUERY_INTERRUPTED;
      error->sqlstate = "70100";
      error->message = "Query execution was interrupted";
      return;
    case SqlRefusal::kPreparing:
      error->sql_errno = ER_LANGUAGE_COMPONENT_INTERNAL;
      error->message = no_sql;
      return;
    case SqlRefusal::kRequestTooLarge:
      error->sql_errno = ER_LANGUAGE_COMPONENT_INTERNAL;
      error->message =
          "the SQL statements in progress in this session hold too much "
          "memory for this one (its text and parameter values)";
      return;
    case SqlRefusal::kNonFiniteParameter:
      error->sql_errno = ER_WRONG_ARGUMENTS;
      error->message =
          "a parameter is a number that is not finite (NaN or an infinity)";
      return;
    case SqlRefusal::kReplyTooLarge:
      error->sql_errno = ER_LANGUAGE_COMPONENT_INTERNAL;
      error->message = "the result is larger than MLE allows";
      return;
  }
}

SqlRunner::SqlRunner(MYSQL_THD thd, SqlBudget *budget, ZoneCache *zones)
    : m_thd(thd), m_budget(budget), m_zones(zones) {}

SqlRunner::~SqlRunner() { m_statements.clear(); }

SqlRunner::Statement *SqlRunner::find(uint64_t id) {
  const auto it = m_statements.find(id);
  return it == m_statements.end() ? nullptr : it->second.get();
}

void SqlRunner::close(uint64_t id) { m_statements.erase(id); }

void SqlRunner::send_home(uint64_t id) {
  const auto it = m_statements.find(id);
  if (it == m_statements.end()) return;
  if (it->second->prepared)
    it->second->discard();
  else
    m_statements.erase(it);
}

bool SqlRunner::over_budget(const Statement *except, SqlResultMsg *result) {
  const size_t others =
      m_budget->results_held - (except != nullptr ? except->held : 0);
  if (others < sql_call_results_max()) return false;
  sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
            "the results of the open statements hold too much memory "
            "(mle.sql_call_results_max): read or close them first");
  return true;
}

bool SqlRunner::too_many_statements(SqlResultMsg *result) {
  if (m_budget->statements < sql_statements_max()) return false;
  sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
            "too many open statements (mle.sql_statements_max)");
  return true;
}

void SqlRunner::count_results(Statement &s) {
  // What the server really holds for all results of the execution (rows,
  // metadata, overhead). A statement keeps its results until it is closed
  // or executed again, read or not; mysqld's memory for them is outside the
  // worker's limits, so the session has a budget for it. A server without
  // the attribute: not counted (each result is still bounded by
  // mle.sql_result_max).
  size_t now = 0;
#ifdef MYSQL_STMT_ATTR_RESULT_MEMORY
  if (!s.to_client &&
      mysql_service_mysql_stmt_attributes->get(
          s.h, cstr(MYSQL_STMT_ATTR_RESULT_MEMORY), &now))
    now = 0;
#endif
  if (s.budget == nullptr) return;
  s.budget->results_held += now - s.held;
  s.held = now;
}

void SqlRunner::clear_pending_error() {
  if (!m_pending) return;
  clear_diagnostics(m_thd);
  m_pending = false;
}

void SqlRunner::statement_error(Statement &s, SqlResultMsg *result) {
  uint64_t err_no = 0;
  mysql_cstring_with_length msg{"", 0}, state{"", 0};
  if (mysql_service_mysql_stmt_diagnostics->error_number(s.h, &err_no) ||
      mysql_service_mysql_stmt_diagnostics->error(s.h, &msg) ||
      mysql_service_mysql_stmt_diagnostics->sqlstate(s.h, &state) ||
      err_no == 0 || err_no > mle::wire::kMaxSqlErrno) {
    sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
              "the statement failed without diagnostics");
    return;
  }
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kError;
  result->sql_errno = static_cast<uint32_t>(err_no);
  result->sqlstate.assign(state.str, state.length);
  if (result->sqlstate.size() != 5 || !mle::is_valid_sqlstate(result->sqlstate))
    result->sqlstate = "HY000";
  // The server's message can quote column bytes.
  result->message.assign(msg.str, msg.length);
  repair_utf8(&result->message);
  // The error is in the caller's diagnostics area now.
  m_pending = true;
  m_pending_id = m_error_id;
}

void SqlRunner::handle(const mle::SqlMsg &request, SqlResultMsg *result,
                       mle::DirectReply *direct, uint64_t error_id) {
  // The worker sent another message: it went on after an error it got, so
  // that error must not stay in the caller's diagnostics area.
  clear_pending_error();
  struct Scope {
    mle::DirectReply **slot;
    ~Scope() { *slot = nullptr; }
  } scope{&m_direct};
  m_direct = direct;
  m_error_id = error_id;
  const uint64_t id = request.stmt_id;
  Statement *const s = find(id);

  // Rule 1: CLOSE closes, whatever the state. Rule 2: an action that is not
  // allowed in the id's state gets an ERROR and changes nothing.
  const auto not_allowed = [&](const char *why) {
    sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL, why);
    if (request.action == SqlAction::kExecuteMany) result->failed_row = 0;
  };
  switch (request.action) {
    case SqlAction::kClose:
      close(id);
      *result = SqlResultMsg();
      result->kind = SqlResultKind::kOk;
      return;
    case SqlAction::kExecute:
      if (s != nullptr) return not_allowed("the statement id is in use");
      execute_text(request, result);
      break;
    case SqlAction::kPrepare:
      if (s != nullptr) return not_allowed("the statement id is in use");
      prepare(request, result);
      break;
    case SqlAction::kExecutePrepared:
      if (s == nullptr || !s->prepared)
        return not_allowed("no prepared statement with this id");
      execute_prepared(request, *s, result);
      break;
    case SqlAction::kExecuteMany:
      if (s == nullptr || !s->prepared)
        return not_allowed("no prepared statement with this id");
      execute_many(request, *s, result);
      break;
    case SqlAction::kFetch:
      if (s == nullptr || s->state != Statement::State::kRows)
        return not_allowed("no rows to fetch for this statement id");
      batch(id, *s,
            request.max_rows != 0 ? request.max_rows : sql_chunk_rows(), 0,
            result);
      break;
    case SqlAction::kNextResult:
      // Nothing to move to: END, and the id stays as it is.
      if (s == nullptr || s->state == Statement::State::kIdle) {
        *result = SqlResultMsg();
        result->kind = SqlResultKind::kEnd;
        return;
      }
      next_result(request, *s, result);
      break;
  }

  // Rule 3: an ERROR to an allowed action, whatever its reason, sends the id
  // home. (The statement may be closed by now: only the id is used.)
  const bool written = direct != nullptr && direct->length > 0;
  if (!written && result->kind == SqlResultKind::kError) {
    send_home(id);
    if (request.action == SqlAction::kExecuteMany &&
        result->failed_row == mle::wire::kNoRow)
      result->failed_row = 0;
  }
}

void SqlRunner::refused(SqlRefusal why, bool ran, SqlAction action,
                        uint64_t stmt_id, SqlResultMsg *error) {
  if (action == SqlAction::kClose) {
    close(stmt_id);
    return;
  }
  refusal_error(why, "SQL cannot run while a routine is loaded", error);
  // A reply that is replaced never reaches the worker: nor do the columns
  // it carried.
  if (ran)
    if (Statement *s = find(stmt_id)) s->worker_has_columns = false;
  // The reply is an ERROR: by rule 3 if the action ran or is allowed in the
  // id's state (the id goes home, its results are discarded), else by rule
  // 2 (nothing changes).
  switch (action) {
    case SqlAction::kExecute:
    case SqlAction::kPrepare:
      // Allowed only for an id that is CLOSED; an ERROR leaves it CLOSED,
      // also when the statement was opened meanwhile.
      if (ran) close(stmt_id);
      return;
    case SqlAction::kExecutePrepared:
    case SqlAction::kExecuteMany:
      if (const Statement *s = find(stmt_id); s != nullptr && s->prepared)
        send_home(stmt_id);
      return;
    case SqlAction::kFetch:
      if (const Statement *s = find(stmt_id);
          ran || (s != nullptr && s->state == Statement::State::kRows))
        send_home(stmt_id);
      return;
    case SqlAction::kNextResult:
      send_home(stmt_id);  // allowed in every state
      return;
    case SqlAction::kClose:
      return;
  }
}

// ------------------------------------------------------------- statements

namespace {

/** A new statement handle with our attributes; nullptr on failure. A
    prepared one (parameters, PREPARE) runs without a cursor: execution
    collects the whole result, within `capacity`, with no temporary table
    and no row-by-row fetches, like a direct statement. A server without the
    statement service's "use_cursor" attribute keeps the cursor, rows up to
    mle.sql_row_max (*buffered = false). */
my_h_statement new_statement(bool to_client, size_t capacity,
                             bool prepared = false, bool *buffered = nullptr) {
  my_h_statement h = nullptr;
  if (mysql_service_mysql_stmt_factory->init(&h)) return nullptr;
  if (prepared && !to_client && set_attribute(h, "use_cursor", false)) {
    capacity = sql_row_max();
    if (buffered != nullptr) *buffered = false;
  }
  // The value must be a mysql_cstring_with_length (a char* crashes). The
  // server converts every character value of a result to utf8mb4 and takes
  // text parameters as utf8mb4 (§7.1).
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

/** Binds value v to marker index; the server converts it to what the
    marker needs. */
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
      // A VECTOR binds as its binary floats.
      const auto &f = v.as_vector();
      return bind(false, MYSQL_SP_ARG_TYPE_BLOB, false, f.data(),
                  f.size() * sizeof(float));
    }
    case Tag::kTimestamp: {
      // An instant: UTC fields with a zero displacement, which the server
      // converts to the session's zone. The zero timestamp is no instant.
      mle_time m{};
      m.time_type = MYSQL_TIMESTAMP_TYPE_DATETIME;
      if (v.as_timestamp() != mle::wire::kZeroTimestamp) {
        const mle::Temporal t = utc_fields(v.as_timestamp());
        m.year = t.year;
        m.month = t.month;
        m.day = t.day;
        m.hour = t.hour;
        m.minute = t.minute;
        m.second = t.second;
        m.second_part = t.microsecond;
        m.time_type = MYSQL_TIMESTAMP_TYPE_DATETIME_TZ;
        m.time_zone_displacement = 0;
      }
      return bind(false, MYSQL_SP_ARG_TYPE_DATETIME, false, &m, sizeof m);
    }
    case Tag::kDate:
    case Tag::kTime:
    case Tag::kDatetime: {
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

/** values[from, from + n) to markers 0..n-1. */
bool bind_values(my_h_statement h, const std::vector<Value> &values,
                 size_t from, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (bind_value(h, static_cast<uint32_t>(i), values[from + i])) return true;
  return false;
}

}  // namespace

bool SqlRunner::refused_here(SqlResultMsg *result) {
  // The server refuses statements in stored functions and triggers, but its
  // check for direct statements misses a function called by a trigger
  // (in_sub_stmt compared with ==, not as a mask); the prepared path checks
  // it right. Ask that path once per request (and again while it refuses,
  // so that each refusal is reported).
  if (m_context_checked && !m_refused) return false;
  m_context_checked = true;
  {
    Statement probe;
    probe.h = new_statement(false, 500);
    if (probe.h == nullptr) return false;
    const mysql_cstring_with_length text = cstr("DO 0");
    m_refused = mysql_service_mysql_stmt_execute->prepare(text, probe.h);
    if (m_refused) statement_error(probe, result);
  }  // closed while its error is in the diagnostics area (closing a failed
     // statement adds an error of its own to an empty one)
  if (m_refused) explain_refusal(result);
  return m_refused;
}

void SqlRunner::statement_failed(std::unique_ptr<Statement> s,
                                 SqlResultMsg *result) {
  statement_error(*s, result);
  s.reset();  // closed while its error is in the diagnostics area
  explain_refusal(result);
}

void SqlRunner::explain_refusal(SqlResultMsg *result) {
  if (result->kind != SqlResultKind::kError ||
      result->sql_errno != ER_STMT_EXECUTION_NOT_ALLOWED_WITHIN_SP_OR_TRG_OR_UDF)
    return;
  // The server's text names its statement interface ("Prepared Statement
  // Handle Interface"), which tells a routine's author nothing: the same
  // error, in their terms. It leaves the diagnostics area; handed back, it
  // is reported as the routine's error, with where it happened.
  clear_pending_error();
  result->message =
      "SQL statements are not allowed in stored functions and triggers: "
      "use a stored procedure";
}

void SqlRunner::execute_text(const mle::SqlMsg &request,
                             SqlResultMsg *result) {
  if (request.params.empty() && refused_here(result)) return;
  if (too_many_statements(result) || over_budget(nullptr, result)) return;
  const bool to_client =
      (request.flags & mle::wire::kPassResultToClient) != 0;
  const bool with_params = !request.params.empty();
  auto s = std::make_unique<Statement>();
  // The server buffers the whole result (buffer_capacity bytes), with or
  // without parameters (no cursor: new_statement).
  s->h = new_statement(to_client, sql_result_max(), with_params, &s->buffered);
  if (s->h == nullptr)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "cannot create a statement");
  s->to_client = to_client;
  const mysql_cstring_with_length text{request.statement.data(),
                                       request.statement.size()};
  const bool timed = sql_exec_time();
  const uint64_t t0 = timed ? now_us() : 0;
  bool failed;
  if (with_params) {
    failed = mysql_service_mysql_stmt_execute->prepare(text, s->h);
    if (!failed) {
      // A one-shot prepared execution: exactly one value per marker.
      uint32_t markers = 0;
      (void)mysql_service_mysql_stmt_metadata->param_count(s->h, &markers);
      if (request.params.size() != markers)
        return sql_error(result, ER_WRONG_ARGUMENTS,
                         request.params.size() > markers
                             ? "more values than parameter markers"
                             : "fewer values than parameter markers");
      failed = bind_values(s->h, request.params, 0, request.params.size()) ||
               mysql_service_mysql_stmt_execute->execute(s->h);
    }
  } else {
    failed = mysql_service_mysql_stmt_execute_direct->execute(text, s->h);
  }
  // A failed EXECUTE never gets its id.
  if (failed) return statement_failed(std::move(s), result);
  Statement &st = *s;
  st.budget = m_budget;
  m_budget->statements++;
  count_results(st);
  m_statements[request.stmt_id] = std::move(s);
  reply(request.stmt_id, st,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(),
        timed ? now_us() - t0 : 0, result);
}

void SqlRunner::prepare(const mle::SqlMsg &request, SqlResultMsg *result) {
  if (too_many_statements(result)) return;
  const bool to_client =
      (request.flags & mle::wire::kPassResultToClient) != 0;
  auto s = std::make_unique<Statement>();
  s->h = new_statement(to_client, sql_result_max(), true, &s->buffered);
  if (s->h == nullptr)
    return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                     "cannot create a statement");
  s->prepared = true;
  s->to_client = to_client;
  const mysql_cstring_with_length text{request.statement.data(),
                                       request.statement.size()};
  if (mysql_service_mysql_stmt_execute->prepare(text, s->h))
    return statement_failed(std::move(s), result);
  (void)mysql_service_mysql_stmt_metadata->param_count(s->h, &s->param_count);
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kOk;
  result->param_count = s->param_count;
  s->budget = m_budget;
  m_budget->statements++;
  m_statements[request.stmt_id] = std::move(s);
}

void SqlRunner::execute_prepared(const mle::SqlMsg &request, Statement &s,
                                 SqlResultMsg *result) {
  // All pending results of the id go first; an ERROR from here on leaves it
  // IDLE.
  s.discard();
  if (request.params.size() != s.param_count)
    return sql_error(result, ER_WRONG_ARGUMENTS,
                     request.params.size() > s.param_count
                         ? "more values than parameter markers"
                         : "fewer values than parameter markers");
  if (over_budget(&s, result)) return;  // its own results go when it runs
  const bool timed = sql_exec_time();
  const uint64_t t0 = timed ? now_us() : 0;
  const bool failed =
      bind_values(s.h, request.params, 0, request.params.size()) ||
      mysql_service_mysql_stmt_execute->execute(s.h);
  count_results(s);
  if (failed) return statement_error(s, result);
  reply(request.stmt_id, s,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(),
        timed ? now_us() - t0 : 0, result);
}

void SqlRunner::execute_many(const mle::SqlMsg &request, Statement &s,
                             SqlResultMsg *result) {
  // The prepared statement once per row, in the caller's session and
  // transaction, without a round trip to the worker in between. It stops at
  // the first failing row; the rows before it stay executed (the
  // transaction is the caller's). In every ERROR failed_row is the number
  // of rows executed before it.
  s.discard();
  const auto refuse = [&](uint32_t sql_errno, const std::string &why) {
    sql_error(result, sql_errno, why);
    result->failed_row = 0;
  };
  if (s.to_client)
    return refuse(ER_WRONG_ARGUMENTS,
                  "executeMany cannot send results to the client");
  if (request.row_width == 0 || request.row_width != s.param_count ||
      request.params.size() % request.row_width != 0)
    return refuse(ER_WRONG_ARGUMENTS,
                  "each row of executeMany must have one value per parameter "
                  "marker, and the statement at least one marker (it has " +
                      std::to_string(s.param_count) + ")");
  if (s.returns_rows)
    return refuse(ER_WRONG_ARGUMENTS,
                  "executeMany runs statements that return no rows; this one "
                  "returns a result set");
  if (over_budget(&s, result)) {
    result->failed_row = 0;
    return;
  }
  // However it ends: the statement's earlier results are gone, and what it
  // holds now (a refused result set) is counted.
  struct Counted {
    SqlRunner *runner;
    Statement &statement;
    ~Counted() { runner->count_results(statement); }
  } counted{this, s};
  const uint32_t width = request.row_width;
  const size_t rows = request.params.size() / width;
  const bool timed = sql_exec_time();
  const uint64_t t0 = timed ? now_us() : 0;
  uint64_t affected = 0;
  for (size_t i = 0; i < rows; i++) {
    const auto failed_at_row = [&] {
      result->failed_row = static_cast<uint32_t>(i);
    };
    if (bind_values(s.h, request.params, i * width, width) ||
        mysql_service_mysql_stmt_execute->execute(s.h)) {
      statement_error(s, result);
      return failed_at_row();
    }
    // A row's execution can yield several results: their counts add up.
    while (true) {
      uint32_t fields = 0;
      (void)mysql_service_mysql_stmt_resultset_metadata->field_count(s.h,
                                                                     &fields);
      if (fields != 0) {
        // The row was executed, but it counts as failing.
        s.returns_rows = true;
        sql_error(result, ER_WRONG_ARGUMENTS,
                  "executeMany runs statements that return no rows; this one "
                  "returned a result set (the row was executed)");
        return failed_at_row();
      }
      uint64_t n = 0;
      (void)mysql_service_mysql_stmt_diagnostics->affected_rows(s.h, &n);
      affected += n;
      bool more = false;
      if (mysql_service_mysql_stmt_result->next_result(s.h, &more)) {
        statement_error(s, result);
        return failed_at_row();
      }
      if (!more) break;
    }
  }
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kOk;
  result->affected_rows = affected;
  result->exec_us = timed ? now_us() - t0 : 0;
  if (rows > 0) {  // the last row's
    (void)mysql_service_mysql_stmt_diagnostics->insert_id(s.h,
                                                          &result->insert_id);
    read_warnings(s.h, &result->warnings);
  }
}

void SqlRunner::next_result(const mle::SqlMsg &request, Statement &s,
                            SqlResultMsg *result) {
  if (s.state == Statement::State::kRows) {
    // The rest of the current result set goes.
    my_h_row row = nullptr;
    while (!s.next_row(&row) && row != nullptr) {
    }
    s.pending = nullptr;
    s.all.clear();
    s.next = 0;
    s.advance();
    if (!s.next_pending) {
      *result = SqlResultMsg();
      result->kind = SqlResultKind::kEnd;
      send_home(request.stmt_id);
      return;
    }
  }
  s.next_pending = false;
  if (s.next_failed) {
    // Not the end of the results: the error the server gave for the next.
    s.next_failed = false;
    return statement_error(s, result);
  }
  reply(request.stmt_id, s,
        request.max_rows != 0 ? request.max_rows : sql_chunk_rows(), 0,
        result);
}

// ------------------------------------------------------------- results

void SqlRunner::reply(uint64_t id, Statement &s, uint32_t max_rows,
                      uint64_t exec_us, SqlResultMsg *result) {
  *result = SqlResultMsg();
  uint32_t fields = 0;
  (void)mysql_service_mysql_stmt_resultset_metadata->field_count(s.h, &fields);
  if (s.to_client || fields == 0) {
    // A row count; or the server has sent every result of the execution to
    // the consumer of the statement that invoked the routine: one OK, and
    // the id goes home (§10.5).
    result->kind = SqlResultKind::kOk;
    result->exec_us = exec_us;
    (void)mysql_service_mysql_stmt_diagnostics->affected_rows(
        s.h, &result->affected_rows);
    (void)mysql_service_mysql_stmt_diagnostics->insert_id(s.h,
                                                          &result->insert_id);
    read_warnings(s.h, &result->warnings);
    if (!s.to_client) s.advance();
    if (!s.to_client && s.next_pending) {
      result->more_results = true;
      s.state = Statement::State::kNext;
    } else {
      send_home(id);  // may close s
    }
    return;
  }

  // A result set: its columns, then the first batch. A prepared id whose
  // set has exactly the columns sent last for the id sends none (§10.3).
  s.returns_rows = true;
  s.omit_columns = s.prepared && s.worker_has_columns && s.same_columns(fields);
  bool timestamps = false;
  if (s.omit_columns) {
    timestamps = std::find(s.readers.begin(), s.readers.end(),
                           Reader::kTimestamp) != s.readers.end();
    fields = 0;  // nothing to build
  } else {
    s.worker_has_columns = false;
    s.columns.clear();
    s.readers.clear();
    s.raw.clear();
  }
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
    const auto length =
        static_cast<uint32_t>(number_info<unsigned long>(f, "length"));
    const uint32_t decimals = number_info<uint>(f, "decimals");
    c.type.is_unsigned = is_unsigned;
    c.type.type_name = sql_type_name(type);
    // As for a routine's parameters: text and bytes differ by the charset.
    if (binary) {
      if (c.type.type_name == "VARCHAR") c.type.type_name = "VARBINARY";
      if (c.type.type_name == "CHAR") c.type.type_name = "BINARY";
    } else if (c.type.type_name == "BLOB") {
      c.type.type_name = "TEXT";
    }
    Reader r;
    switch (type) {
      case MYSQL_SP_ARG_TYPE_NULL:
        r = Reader::kNull;
        c.type.tag = Tag::kNull;
        c.type.is_unsigned = false;
        break;
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
        c.type.length = length;
        break;
      case MYSQL_SP_ARG_TYPE_FLOAT:
        r = Reader::kFloat;
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
        c.type.scale = decimals;
        c.type.precision = decimal_precision(length, decimals, is_unsigned);
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
        c.type.scale = decimals <= 6 ? decimals : 0;
        break;
      case MYSQL_SP_ARG_TYPE_DATETIME:
      case MYSQL_SP_ARG_TYPE_DATETIME2:
        r = Reader::kDatetime;
        c.type.tag = Tag::kDatetime;
        c.type.scale = decimals <= 6 ? decimals : 0;
        break;
      case MYSQL_SP_ARG_TYPE_TIMESTAMP:
      case MYSQL_SP_ARG_TYPE_TIMESTAMP2:
        r = Reader::kTimestamp;
        c.type.tag = Tag::kTimestamp;
        c.type.scale = decimals <= 6 ? decimals : 0;
        timestamps = true;
        break;
      case MYSQL_SP_ARG_TYPE_JSON:
        r = Reader::kJson;
        c.type.tag = Tag::kJson;
        break;
      case MYSQL_SP_ARG_TYPE_VECTOR:
        r = Reader::kVector;
        c.type.tag = Tag::kVector;
        c.type.length = length / 4;
        break;
      default: {  // strings, BLOBs, ENUM, SET, GEOMETRY
        // ENUM and SET are text whatever their character set.
        const bool bytes = binary && type != MYSQL_SP_ARG_TYPE_ENUM &&
                           type != MYSQL_SP_ARG_TYPE_SET;
        r = bytes ? Reader::kBytes : Reader::kText;
        c.type.tag = bytes ? Tag::kBytes : Tag::kString;
        c.type.length = length;
        if (c.type.type_name.empty())
          c.type.type_name = bytes ? "VARBINARY" : "VARCHAR";
        break;
      }
    }
    s.columns.push_back(std::move(c));
    s.readers.push_back(r);
    s.raw.push_back({type, number_info<unsigned long>(f, "length"), decimals,
                     number_info<uint>(f, "flags"), is_unsigned});
  }
  s.state = Statement::State::kRows;
  s.columns_sent = false;
  s.pending = nullptr;
  s.all.clear();
  s.next = 0;
  s.total = mle::wire::kUnknownTotalRows;
  // The server gives a TIMESTAMP as local time in the session's zone.
  if (timestamps) {
    std::string name;
    const TimeZone *zone = m_zones->current(m_thd, &name);
    if (zone == nullptr)
      return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                       "cannot convert TIMESTAMP values: the session's time "
                       "zone '" + name + "' is not known to MLE");
    s.zone = *zone;
  }
  if (s.buffered) {
    // The server has all rows already: their handles give the count, which
    // lets a worker allocate the routine's array once.
    my_h_row row = nullptr;
    while (true) {
      if (mysql_service_mysql_stmt_result->fetch(s.h, &row))
        return statement_error(s, result);
      if (row == nullptr) break;
      s.all.push_back(row);
    }
    s.total = s.all.size();
  }
  // batch() may close (destroy) s: nothing touches it afterwards.
  batch(id, s, max_rows, exec_us, result);
}

bool SqlRunner::finish_result_set(uint64_t id, Statement &s) {
  s.advance();
  if (s.next_pending) {
    s.state = Statement::State::kNext;
    return true;
  }
  send_home(id);  // may close s
  return false;
}

namespace {

/** What reading a row needs. */
struct RowSource {
  const std::vector<Reader> &readers;
  TimeZone &zone;
  /** Set when a TIMESTAMP has no instant. */
  bool bad_timestamp = false;
};

mle::Temporal temporal_of(const mle_time &t) {
  mle::Temporal tm;
  tm.year = static_cast<uint16_t>(t.year);
  tm.month = static_cast<uint8_t>(t.month);
  tm.day = static_cast<uint8_t>(t.day);
  tm.hour = t.hour;
  tm.minute = static_cast<uint8_t>(t.minute);
  tm.second = static_cast<uint8_t>(t.second);
  tm.microsecond = static_cast<uint32_t>(t.second_part);
  tm.negative = t.neg;
  return tm;
}

uint64_t bit_value(std::string_view bytes) {
  uint64_t x = 0;  // big-endian bytes
  for (unsigned char c : bytes) x = (x << 8) | c;
  return x;
}

/** One row's values, each NULL or with exactly its column's tag; false on a
    getter failure or a TIMESTAMP without an instant. */
bool read_row(my_h_row row, RowSource &from, std::vector<Value> *out) {
  for (uint32_t i = 0; i < from.readers.size(); i++) {
    bool is_null = false;
    bool failed = false;
    Value v;
    switch (from.readers[i]) {
      case Reader::kNull:
        is_null = true;
        break;
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
        v = from.readers[i] == Reader::kUnsigned
                ? Value::u64(x)
                : Value::i64(static_cast<int64_t>(x));
        break;
      }
      case Reader::kFloat:
      case Reader::kDouble: {
        double x = 0;
        failed = mysql_service_mysql_stmt_get_double->get(row, i, &x, &is_null);
        v = from.readers[i] == Reader::kFloat
                ? Value::f32(static_cast<float>(x))
                : Value::f64(x);
        break;
      }
      case Reader::kDate:
      case Reader::kTime:
      case Reader::kDatetime:
      case Reader::kTimestamp: {
        mle_time t{};
        failed = mysql_service_mysql_stmt_get_time->get(row, i, &t, &is_null);
        if (failed || is_null) break;
        const mle::Temporal tm = temporal_of(t);
        if (from.readers[i] == Reader::kTimestamp) {
          int64_t at = 0;
          if (!from.zone.instant(tm, &at)) {
            from.bad_timestamp = true;
            return false;
          }
          v = Value::timestamp(at);
        } else {
          v = Value::temporal(from.readers[i] == Reader::kDate   ? Tag::kDate
                              : from.readers[i] == Reader::kTime ? Tag::kTime
                                                                 : Tag::kDatetime,
                              tm);
        }
        break;
      }
      default: {
        mysql_cstring_with_length s{nullptr, 0};
        failed = mysql_service_mysql_stmt_get_string->get(row, i, &s, &is_null);
        if (failed || is_null) break;
        if (from.readers[i] == Reader::kVector) {
          std::vector<float> f(s.length / sizeof(float));
          memcpy(f.data(), s.str, f.size() * sizeof(float));
          v = Value::vector(std::move(f));
          break;
        }
        std::string text(s.str, s.length);
        switch (from.readers[i]) {
          case Reader::kDecimal:
            v = Value::decimal(std::move(text));
            break;
          case Reader::kJson:
            v = Value::json(std::move(text));
            break;
          case Reader::kBytes:
            v = Value::bytes(std::move(text));
            break;
          case Reader::kBit:
            v = Value::u64(bit_value(text));
            break;
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

/** One row, written as read_row's Values would be encoded (Writer::value),
    without them; false as read_row. The caller checks w->overflow(). */
bool write_row(my_h_row row, RowSource &from, mle::Writer *w) {
  for (uint32_t i = 0; i < from.readers.size(); i++) {
    bool is_null = false;
    bool failed = false;
    switch (from.readers[i]) {
      case Reader::kNull:
        is_null = true;
        break;
      case Reader::kSigned: {
        int64_t x = 0;
        failed = mysql_service_mysql_stmt_get_integer->get(row, i, &x, &is_null);
        if (!failed && !is_null) w->i64_value(x);
        break;
      }
      case Reader::kUnsigned:
      case Reader::kUnsignedSmall: {
        uint64_t x = 0;
        failed = mysql_service_mysql_stmt_get_unsigned_integer->get(row, i, &x,
                                                                    &is_null);
        if (failed || is_null) break;
        if (from.readers[i] == Reader::kUnsigned)
          w->u64_value(x);
        else
          w->i64_value(static_cast<int64_t>(x));
        break;
      }
      case Reader::kFloat:
      case Reader::kDouble: {
        double x = 0;
        failed = mysql_service_mysql_stmt_get_double->get(row, i, &x, &is_null);
        if (failed || is_null) break;
        if (from.readers[i] == Reader::kFloat)
          w->f32_value(static_cast<float>(x));
        else
          w->f64_value(x);
        break;
      }
      case Reader::kDate:
      case Reader::kTime:
      case Reader::kDatetime:
      case Reader::kTimestamp: {
        mle_time t{};
        failed = mysql_service_mysql_stmt_get_time->get(row, i, &t, &is_null);
        if (failed || is_null) break;
        const mle::Temporal tm = temporal_of(t);
        if (from.readers[i] == Reader::kTimestamp) {
          int64_t at = 0;
          if (!from.zone.instant(tm, &at)) {
            from.bad_timestamp = true;
            return false;
          }
          w->timestamp_value(at);
        } else {
          w->temporal_value(from.readers[i] == Reader::kDate   ? Tag::kDate
                            : from.readers[i] == Reader::kTime ? Tag::kTime
                                                               : Tag::kDatetime,
                            tm);
        }
        break;
      }
      default: {
        mysql_cstring_with_length s{nullptr, 0};
        failed = mysql_service_mysql_stmt_get_string->get(row, i, &s, &is_null);
        if (failed || is_null) break;
        const std::string_view text(s.str, s.length);
        switch (from.readers[i]) {
          case Reader::kVector:
            // The server's bytes, once, into the arena.
            w->vector_value(s.str,
                            static_cast<uint32_t>(s.length / sizeof(float)));
            break;
          case Reader::kDecimal:
            w->text_value(Tag::kDecimal, text);
            break;
          case Reader::kJson:
            w->text_value(Tag::kJson, text);
            break;
          case Reader::kBytes:
            w->text_value(Tag::kBytes, text);
            break;
          case Reader::kBit:
            w->u64_value(bit_value(text));
            break;
          default:
            w->text_value(Tag::kString, text);
        }
      }
    }
    if (failed) return false;
    if (is_null) w->null_value();
  }
  return true;
}

}  // namespace

void SqlRunner::batch(uint64_t id, Statement &s, uint32_t max_rows,
                      uint64_t exec_us, SqlResultMsg *result) {
  Batch b;
  b.max_rows = std::max<uint32_t>(max_rows, 1);
  b.exec_us = exec_us;
  b.first = !s.columns_sent;
  // The set's columns go with its first batch, unless the worker has them.
  b.columns = b.first && !s.omit_columns;
  // The statement has run (no cursor), so its warnings are final: they go
  // with the set's first batch, so that a worker has them before it reads
  // all rows, and with its last, which carries the complete list.
  read_warnings(s.h, &b.warnings);
  const auto row_error = [&] {
    if (b.bad_timestamp)
      return sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                       "a TIMESTAMP value of the result cannot be converted "
                       "from the session's time zone");
    statement_error(s, result);
  };
  if (m_direct != nullptr) {
    switch (write_rows(id, s, b)) {
      case Direct::kWritten:
        // An ERROR instead (nothing was written): the room kept was short,
        // and the columns did not go either.
        if (m_direct->length == 0) {
          if (Statement *left = find(id)) left->worker_has_columns = false;
          sql_error(result, ER_LANGUAGE_COMPONENT_INTERNAL,
                    "an SQL result did not fit the room kept for it");
        }
        return;
      case Direct::kError:
        return row_error();
      case Direct::kNotHere:
        break;
    }
  }
  // One row, as Values: a batch of one row may be larger than the arena
  // (the message then goes in several chunks).
  *result = SqlResultMsg();
  result->kind = SqlResultKind::kRows;
  result->exec_us = exec_us;
  if (b.columns)
    result->columns = s.columns;
  else
    result->ncolumns = static_cast<uint32_t>(s.columns.size());
  result->total_rows = s.total;
  my_h_row row = s.pending;
  s.pending = nullptr;
  if (row == nullptr && s.next_row(&row)) return statement_error(s, result);
  bool more = false;
  if (row != nullptr) {
    RowSource from{s.readers, s.zone};
    if (!read_row(row, from, &result->rows)) {
      b.bad_timestamp = from.bad_timestamp;
      return row_error();
    }
    m_zones->learn(s.zone);
    // Whether another row follows; the server keeps this one until then.
    my_h_row next = nullptr;
    if (s.next_row(&next)) return statement_error(s, result);
    s.pending = next;
    more = next != nullptr;
  }
  s.columns_sent = true;
  if (b.columns) s.worker_has_columns = s.prepared;
  if (more) {
    result->more = true;
    if (s.buffered && b.first) result->warnings = std::move(b.warnings);
    return;
  }
  result->warnings = std::move(b.warnings);
  result->more_results = finish_result_set(id, s);
}

SqlRunner::Direct SqlRunner::write_rows(uint64_t id, Statement &s, Batch &b) {
  // Room for the warnings at the end of the batch is kept from the start.
  size_t tail = 1 + 1 + 4 + 8;
  for (const mle::Warning &x : b.warnings)
    tail += 4 + 4 + 4 + x.message.size();
  if (tail >= m_direct->capacity) return Direct::kNotHere;

  m_direct->posts_at_start = *m_direct->posts;
  mle::Writer w(m_direct->data, m_direct->capacity - tail);
  static const std::vector<mle::Column> kNoColumns;
  const size_t count_at =
      mle::put_rows_head(w, b.columns ? s.columns : kNoColumns, s.total);
  if (w.overflow()) return Direct::kNotHere;
  uint32_t rows = 0;
  bool more = false;
  RowSource from{s.readers, s.zone};
  while (true) {
    my_h_row row = s.pending;
    s.pending = nullptr;
    if (row == nullptr) {
      if (s.next_row(&row)) return Direct::kError;
      if (row == nullptr) break;
    }
    if (rows >= b.max_rows) {
      s.pending = row;
      more = true;
      break;
    }
    const size_t at = w.position();
    if (!write_row(row, from, &w)) {
      b.bad_timestamp = from.bad_timestamp;
      return Direct::kError;
    }
    if (w.overflow()) {
      w.set_position(at);
      s.pending = row;
      // The first row does not fit the arena with the head and the tail:
      // it goes alone, as Values.
      if (rows == 0) return Direct::kNotHere;
      more = true;
      break;
    }
    rows++;
  }
  w.patch_u32(count_at, rows);
  const bool with_warnings = !more || (s.buffered && b.first);
  m_zones->learn(s.zone);
  s.columns_sent = true;
  if (b.columns) s.worker_has_columns = s.prepared;
  // The set is complete: whether another result follows. This may close
  // (destroy) s.
  const bool more_results = !more && finish_result_set(id, s);
  w.set_capacity(m_direct->capacity);  // the tail's room
  static const std::vector<mle::Warning> kNone;
  mle::put_rows_tail(w, more, more_results, with_warnings ? b.warnings : kNone,
                     b.exec_us);
  if (w.overflow()) return Direct::kWritten;  // nothing written: the caller's error
  m_direct->length = w.position();
  m_direct->rows = rows;
  return Direct::kWritten;
}

// ------------------------------------------------------------- errors

void SqlRunner::call_succeeded() { clear_pending_error(); }

bool SqlRunner::propagates(const mle::HeldError &held) {
  // The error the server left in the diagnostics area is the one the ERROR
  // with this id was made from.
  if (m_pending && held.error_id == m_pending_id) return true;
  clear_pending_error();
  return false;
}

bool signal_error(MYSQL_THD thd, uint32_t sql_errno, const std::string &state,
                  const std::string &text) {
  // SIGNAL's rules: a 5-character SQLSTATE, an error number 1..65535. Class
  // 00 is no condition and class 01 a warning: neither fails a statement.
  if (state.size() != 5 || !mle::is_valid_sqlstate(state) ||
      state.compare(0, 2, "00") == 0 || state.compare(0, 2, "01") == 0 ||
      sql_errno < 1 || sql_errno > mle::wire::kMaxSqlErrno)
    return false;
  // The message as a hex literal: no quoting or escaping rules apply (also
  // under NO_BACKSLASH_ESCAPES), any bytes are safe in the statement text.
  // MESSAGE_TEXT takes 128 characters: a longer text is cut (whole UTF-8
  // characters) rather than losing the error's number to a failed SIGNAL.
  size_t length = 0;
  for (size_t chars = 0; length < text.size(); chars++) {
    if (chars == 128) break;
    length++;
    while (length < text.size() &&
           (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
      length++;
  }
  static const char kHex[] = "0123456789ABCDEF";
  std::string message;
  for (unsigned char c : std::string_view(text).substr(0, length)) {
    message.push_back(kHex[c >> 4]);
    message.push_back(kHex[c & 15]);
  }
  const std::string statement =
      "SIGNAL SQLSTATE '" + state + "' SET MESSAGE_TEXT = " +
      (message.empty() ? std::string("''") : "_utf8mb4 X'" + message + "'") +
      ", MYSQL_ERRNO = " + std::to_string(sql_errno);
  bool raised = false;
  {
    my_h_statement h = new_statement(false, 500);
    if (h == nullptr) return false;
    if (mysql_service_mysql_stmt_execute_direct->execute(
            {statement.data(), statement.size()}, h)) {
      uint64_t err_no = 0;
      mysql_cstring_with_length msg{"", 0};
      (void)mysql_service_mysql_stmt_diagnostics->error_number(h, &err_no);
      (void)mysql_service_mysql_stmt_diagnostics->error(h, &msg);
      // The refusal of SQL in a function has the same number as the error
      // a routine got for its own statement: the message tells them apart.
      raised = err_no == sql_errno &&
               (err_no != ER_STMT_EXECUTION_NOT_ALLOWED_WITHIN_SP_OR_TRG_OR_UDF ||
                std::string_view(msg.str, msg.length) ==
                    std::string_view(text).substr(0, length));
    }
    // Closed while its error is in the diagnostics area.
    (void)mysql_service_mysql_stmt_factory->close(h);
  }
  // In a function SQL is not allowed, so SIGNAL is not either; or the
  // SIGNAL itself failed: the caller reports the error another way.
  if (!raised) clear_diagnostics(thd);
  return raised;  // if raised, the SIGNAL's error is in the diagnostics area
}

}  // namespace mle_component
