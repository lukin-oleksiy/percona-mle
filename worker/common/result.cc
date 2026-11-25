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

#include "common/result.hpp"

namespace mle::common {

void cut(std::string &s, size_t max) {
  if (s.size() <= max) return;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  s.resize(n);
}

void cap_error(ErrorInfo &e) {
  cut(e.message, wire::kMaxErrorMessage);
  if (e.frames.size() > wire::kMaxStackFrames) e.frames.resize(wire::kMaxStackFrames);
  for (ErrorInfo::Frame &f : e.frames) {
    cut(f.function, wire::kMaxFrameText);
    cut(f.file, wire::kMaxFrameText);
    cut(f.text, wire::kMaxFrameText);
  }
}

size_t error_size(const ErrorInfo &e) {
  size_t n = 4 + e.language.size() + 4 + e.message.size() + 4 + 4 + 4 + e.sql_state.size() + 8;
  for (const ErrorInfo::Frame &f : e.frames) n += 4 + f.function.size() + 4 + f.file.size() + 12 + 4 + f.text.size();
  return n;
}

bool write_result(Writer &wr, std::span<const std::byte> values, std::string_view out, std::string_view err,
                  bool cleared, const ErrorInfo *error) {
  bool fits = wr.bytes(values) && wr.str(out) && wr.str(err) && wr.put<uint8_t>(cleared ? 1 : 0) &&
              wr.put<uint32_t>(0);  // no findings
  if (error == nullptr) return fits && wr.put<uint8_t>(0);
  const ErrorInfo &e = *error;
  fits = fits && wr.put<uint8_t>(1) && wr.str(e.language) && wr.str(e.message) &&
         wr.put(static_cast<uint32_t>(e.frames.size()));
  for (const ErrorInfo::Frame &f : e.frames)
    fits = fits && wr.str(f.function) && wr.str(f.file) && wr.put(f.line) && wr.put(f.column) && wr.put(f.end_column) &&
           wr.str(f.text);
  return fits && wr.put(e.sql_errno) && wr.str(e.sql_state) && wr.put(e.sql_error_id);
}

}  // namespace mle::common
