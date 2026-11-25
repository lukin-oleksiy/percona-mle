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

/*
  The body of a RESULT (§9.4) after its values, for the native workers' C++ writers (the V8
  worker's results, the CPython worker's own failures; MVP review V8-11): console output, findings
  (none), the Error with the caps of §7.4.
*/

#ifndef MLE_WORKER_COMMON_RESULT_HPP
#define MLE_WORKER_COMMON_RESULT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/wire.hpp"

namespace mle::common {

/** An Error of a RESULT (§7.4). */
struct ErrorInfo {
  /** The worker's language, "" (the worker's own error) or a marker (MLE-Type, MLE-TimeZone,
      MLE-Option). */
  std::string language;
  std::string message;
  struct Frame {
    std::string function, file;
    uint32_t line = 0, column = 0, end_column = 0;
    std::string text;
  };
  std::vector<Frame> frames;
  uint32_t sql_errno = 0;
  std::string sql_state;
  /** The error_id of the SQL_RESULT ERROR this error hands back (§10.8), or 0. */
  uint64_t sql_error_id = 0;
};

/** has_return 0 and no out_params: the values of a RESULT that has none (§9.4). */
inline constexpr std::array<std::byte, 5> kNoValues{};

/** Cuts text to at most max bytes at a character boundary (§7.4). */
void cut(std::string &s, size_t max);

/** Applies the caps of §7.4 to e: the message, the number of frames, each frame's texts. An Error
    within them (16 KiB, and no frames outside PREPARE and CALL) also fits the 64 KiB of a RESULT
    of DROP, RESET and OPTIONS. */
void cap_error(ErrorInfo &e);

/** The bytes e takes in a RESULT. */
size_t error_size(const ErrorInfo &e);

/** Writes a RESULT's body: values (has_return, the return value, out_params), the console output
    (cleared: console_cleared), no findings, and error (nullptr: none). False if it does not fit. */
bool write_result(Writer &wr, std::span<const std::byte> values, std::string_view out, std::string_view err,
                  bool cleared, const ErrorInfo *error);

}  // namespace mle::common

#endif  // MLE_WORKER_COMMON_RESULT_HPP
