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
  @file component/routine.h

  A stored program as the component keeps it (the SQL layer's
  external_program_handle): its PREPARE message for the workers, and how
  its arguments are read from and its results stored into the SQL layer
  (mysql_stored_program_* services).

  SQL types and their wire values (docs/protocol.md §7.3):
    TINYINT .. INT, YEAR, BIGINT   I64 (unsigned INT and smaller fit too)
    BIGINT UNSIGNED, BIT    U64
    FLOAT / DOUBLE          F32 / F64
    DECIMAL                 DECIMAL (text)
    CHAR, VARCHAR, TEXT, ENUM, SET   STRING; a parameter or return type must
                            be declared utf8mb4 (protocol Appendix A,
                            Oracle's rule): the server converts values of
                            other character sets
    BINARY, VARBINARY, BLOB   BYTES (GEOMETRY parameters and return types
                            are refused)
    JSON                    JSON
    DATE, TIME, DATETIME    a calendar date and clock time, without a zone
    TIMESTAMP               an instant: microseconds since 1970 UTC
    VECTOR                  VECTOR

  TIMESTAMP: the server's services give and take a TIMESTAMP only as local
  time in the session's time zone, so the instant is computed from it
  (TimeZone): exactly for a zone that is an offset; by the zone's rules for
  SYSTEM and for a named zone, where a local time in the hour a zone repeats
  is taken as its first occurrence and one in an hour the zone skips as the
  instant of the change (the server's rules for a named zone). The session
  keeps the zone and the interval of local time in which its offset is the
  one found last (ZoneCache), so a value costs a comparison and a
  subtraction. An instant a worker returns is given to the server with its
  offset; the server stores it through local time too.
*/

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mysql/components/services/bits/stored_program_bits.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysql/components/services/mysql_string.h>

#include "mle/codec.h"
#include "time_zone.hpp"

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

using mle::repair_utf8;

/** @@session.time_zone of thd (nullptr: the current session): "SYSTEM", an
    offset "+05:30" or a zone name. Empty if the server does not tell. */
std::string session_time_zone(MYSQL_THD thd = nullptr);

/** @@session.time_zone of thd as the server holds it (valid until the
    session's zone changes); empty if the server does not tell. */
std::string_view session_time_zone_name(MYSQL_THD thd = nullptr);

/** A session's current zone, kept between statements (time_zone.hpp). The
    session's zone is read only when a TIMESTAMP is converted, and is
    looked up again only when the server's zone object is another one. */
class ZoneCache {
 public:
  /** The zone of thd's session now; nullptr (and *name, for the message)
      if it is not known to MLE. */
  TimeZone *current(MYSQL_THD thd, std::string *name);
  /** A statement converted with its copy of the zone: what it found. */
  void learn(const TimeZone &zone) { m_zone.learn(zone); }

 private:
  /** The server's name of the zone looked up last: its address is the
      zone's identity. */
  const char *m_server_name = nullptr;
  bool m_looked_up = false;
  std::string m_name;
  bool m_known = false;
  TimeZone m_zone;
};

/** The calendar fields, in UTC, of an instant that is not the zero
    timestamp. */
mle::Temporal utc_fields(int64_t microseconds);

struct Routine {
  /** Server-unique, also the worker's routine id. */
  uint64_t id = 0;
  /** Language bit (mle_component.h) and the worker's language name. */
  unsigned language = 0;
  uint16_t sp_type = 0;
  std::string name;
  std::string qualified_name;
  /** The routine's schema. */
  std::string schema;
  /** The account that wrote the code (the DEFINER), as account_key();
      empty while the routine is created (the author is the current
      account then) and on a server that does not tell. */
  std::string definer_key;
  /** The definer's user and host as the server gives them (for an account
      lookup); empty with definer_key. */
  std::string definer_user, definer_host;
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

  /** The arguments of the running call (OUT parameters as NULL), each with
      the tag of its declared type or NULL. zones: the session's, for
      TIMESTAMP arguments. False after reporting an error. */
  bool read_args(std::vector<mle::Value> *args, ZoneCache *zones) const;

  /** Stores the return value and the OUT/INOUT values of a successful
      call with the server's assignment rules. False after an error was
      reported (e.g. 1406: too long). */
  bool store_results(const mle::ResultMsg &result) const;

  /** "name(1, 'a')" for messages (Oracle: "... in 'myfunc(10)'"). */
  std::string call_text(const std::vector<mle::Value> &args) const;
};

/** The worker's name of a language bit ("js", "python", "java"; "" for
    none or an unknown one). */
const char *worker_language(unsigned bit);

/** A new routine id (also for mle_eval's snippets). */
uint64_t next_routine_id();

/** Display name of a worker error language ("js" -> "JavaScript"). */
const char *display_language(const std::string &worker_language);

/** INT, VARCHAR, ... for a MYSQL_SP_ARG_TYPE_* value ("" if unknown). */
const char *sql_type_name(uint64_t sql_type);

/** A DECIMAL's precision from its display length (digits, sign and point). */
uint32_t decimal_precision(uint64_t display_length, uint32_t scale, bool is_unsigned);

/** Live routine handles. */
long routines_alive();

}  // namespace mle_component

#endif  // COMPONENTS_MLE_COMPONENT_ROUTINE_H
