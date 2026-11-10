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

// A delegated cgroup v2 base for tests that need memory and process limits.
// Run the test binary with
//   systemd-run --user --scope -p Delegate=yes <test>
// otherwise these tests are skipped.

#ifndef MLE_CGROUP_TEST_UTIL_H
#define MLE_CGROUP_TEST_UTIL_H

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

namespace mle_test {

/** A cgroup base for the tests, or "" (with *why) when this process does
    not run in a delegated cgroup of its own. Moves this process into a
    leaf "host" so that the base has no processes. */
inline std::string test_cgroup(std::string *why) {
  static std::string base, reason;
  static bool done = false;
  if (done) return *why = reason, base;
  done = true;
  std::ifstream in("/proc/self/cgroup");
  std::string line;
  std::getline(in, line);
  const std::string dir = "/sys/fs/cgroup" + line.substr(line.find("::") + 2);
  std::ifstream procs(dir + "/cgroup.procs");
  std::vector<std::string> pids;
  for (std::string p; procs >> p;) pids.push_back(p);
  if (pids.size() != 1 || pids[0] != std::to_string(getpid())) {
    reason = dir +
             " has other processes; run under "
             "systemd-run --user --scope -p Delegate=yes";
    return *why = reason, "";
  }
  const std::string host = dir + "/host";
  mkdir(host.c_str(), 0755);
  std::ofstream(host + "/cgroup.procs") << getpid();
  std::ifstream check("/proc/self/cgroup");
  std::getline(check, line);
  if (line.find("/host") == std::string::npos) {
    reason = "cannot create a child cgroup in " + dir;
    return *why = reason, "";
  }
  base = dir;
  return *why = "", base;
}

#define NEED_CGROUP(base)                           \
  std::string why;                                  \
  const std::string base = test_cgroup(&why);       \
  if (base.empty()) {                               \
    GTEST_SKIP() << "no delegated cgroup: " << why; \
  }                                                 \
  static_assert(true)

}  // namespace mle_test

#endif  // MLE_CGROUP_TEST_UTIL_H
