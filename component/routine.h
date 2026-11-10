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

#ifndef COMPONENTS_MLE_COMPONENT_ROUTINE_H
#define COMPONENTS_MLE_COMPONENT_ROUTINE_H

/**
  @file components/mle/component/routine.h

  A stored program as the component keeps it (the SQL layer's
  external_program_handle): its PREPARE message for the workers, and how
  its arguments are read from and its results stored into the SQL layer
  (mysql_stored_program_* services).

  SQL types and their wire values (11-phase4-component.md §5):
    TINYINT .. INT, YEAR    I64 (unsigned INT and smaller fit too)
    BIGINT UNSIGNED, BIT    U64
    FLOAT / DOUBLE          F32 / F64
    DECIMAL                 Decimal (text)
    CHAR, VARCHAR, TEXT, ENUM, SET (utf8mb4)   String
    BINARY, VARBINARY, BLOB (binary)           Bytes
    JSON                    Json
    DATE, TIME, DATETIME, TIMESTAMP            temporal (session time zone)
    VECTOR                  Vector
  Other character sets and GEOMETRY are refused at CREATE (Oracle: strings
  must be utf8mb4 or binary).
*/

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <mysql/components/services/bits/stored_program_bits.h>
#include <mysql/components/services/mysql_stored_program.h>

#include "mle/codec.h"

namespace mle_component {

/** How a value of one parameter (or the return value) is read and stored. */
enum class Kind {
  kInt,
  kUInt,     // unsigned TINYINT .. INT: read as unsigned, sent as I64
  kBigUInt,  // BIGINT UNSIGNED, BIT
  kFloat,
  kDouble,
  kDecimal,
  kString,
  kBytes,
  kJson,
  kDate,
  kTime,
  kDatetime,
  kTimestamp,
  kVector,
};

struct ParamInfo {
  std::string name;
  mle::wire::ParamMode mode = mle::wire::ParamMode::kIn;
  Kind kind = Kind::kInt;
  mle::ParamType type;
};

struct Routine {
  /** Server-unique, also the worker's routine id. */
  uint64_t id = 0;
  /** Language bit (mle_component.h) and the worker's language name. */
  unsigned language = 0;
  uint16_t sp_type = 0;
  std::string name;
  std::string qualified_name;
  /** The routine's schema (libraries of this schema may be imported). */
  std::string schema;
  std::vector<ParamInfo> params;
  std::optional<ParamInfo> ret;
  /** PREPARE for the workers (body form: empty entry). */
  mle::PrepareMsg prepare;

  bool is_function() const {
    return sp_type == MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION;
  }
  /** "JavaScript", "Python", "Java": error prefix (6113 "JavaScript> ..."). */
  const char *display_language() const;

  /** The routine of a stored program, or nullptr after reporting the error
      (unsupported language, type or parameter name). */
  static Routine *create(stored_program_handle sp);
  static void destroy(Routine *routine);

  /** The arguments of the running call (OUT parameters as NULL). False
      after reporting an error. */
  bool read_args(std::vector<mle::Value> *args) const;

  /** Stores the return value and the OUT/INOUT values of a successful
      call. False after an error was reported (e.g. 1406: too long). */
  bool store_results(const mle::ResultMsg &result) const;

  /** "name(1, 'a')" for messages (Oracle: "... in 'myfunc(10)'"). */
  std::string call_text(const std::vector<mle::Value> &args) const;
};

/** Display name of a worker error language ("js" -> "JavaScript"). */
const char *display_language(const std::string &worker_language);

/** INT, VARCHAR, ... for a MYSQL_SP_ARG_TYPE_* value ("" if unknown). */
const char *sql_type_name(uint64_t sql_type);

/** Live routine handles. */
long routines_alive();

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_ROUTINE_H
