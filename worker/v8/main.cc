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

/**
  @file worker/v8/main.cc

  mle_v8_worker: the MLE worker for LANGUAGE JS on V8, the distribution's V8 from libnode, as a
  bare isolate (no Node.js environment). Its worker_class is "v8" (docs/protocol.md, Appendix B.1).

    mle_v8_worker [--class=<name>] [--heap-mb=<n>] [--packages=<dir>] [--icu-data=<dir>]... [--spin=<n>]
                  [--message-mb=<n>] [--dumpable]

  Started by mle_launcher like the other workers: Landlock, the cgroup and the descriptors (3 =
  the region, 4 = control socket) are in place. Here: PR_SET_DUMPABLE 0 unless --dumpable; the
  seccomp filter of mle_worker (worker/bootstrap/seccomp.cc); the region and the control thread
  (§2); then V8, READY and the request loop (worker.cc).

  Exit codes: 0 the host closed the control socket; 1 a protocol failure, or a start that failed
  (after a failed READY when the region could be used); 2 fd 3 is not an MLE region; 20 bad
  arguments; 21 dumpable; 22 seccomp.
*/

#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>

#include "bootstrap.hpp"
#include "worker.hpp"

int main(int argc, char **argv) {
  mle::v8w::Config config;
  bool dumpable = false;
  for (int i = 1; i < argc; i++) {
    const std::string_view s = argv[i];
    const auto value = [&s](std::string_view prefix, std::string_view *out) {
      if (!s.starts_with(prefix)) return false;
      *out = s.substr(prefix.size());
      return true;
    };
    std::string_view v;
    if (s == "--dumpable") {
      dumpable = true;
    } else if (value("--class=", &v)) {
      config.cls = v;
    } else if (value("--heap-mb=", &v)) {
      config.heap_max = std::strtoull(std::string(v).c_str(), nullptr, 10) << 20;
    } else if (value("--packages=", &v)) {
      config.packages = v;
    } else if (value("--icu-data=", &v)) {
      config.icu_data.emplace_back(v);
    } else if (value("--spin=", &v)) {
      config.spin = std::atoi(std::string(v).c_str());
    } else if (value("--message-mb=", &v)) {
      config.message_max = std::strtoull(std::string(v).c_str(), nullptr, 10) << 20;
    } else {
      std::fprintf(stderr, "mle_v8_worker: unknown argument %s\n", argv[i]);
      std::fprintf(stderr,
                   "usage: mle_v8_worker [--class=<name>] [--heap-mb=<n>] [--packages=<dir>] [--icu-data=<dir>]... "
                   "[--spin=<n>] [--message-mb=<n>] [--dumpable]\n");
      return 20;
    }
  }
  if (config.heap_max < (16ULL << 20)) config.heap_max = 16ULL << 20;
  if (!dumpable && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) return 21;
  if (const int err = mle::worker::install_seccomp(); err != 0) {
    std::fprintf(stderr, "mle_v8_worker: seccomp: %s\n", std::strerror(err));
    return 22;
  }
  // §2: the region and the control thread first, then the runtime, then READY. An interrupt is
  // delivered only to a running request, so the worker exists by then.
  mle::common::EngineHooks hooks;
  hooks.interrupt = [](uint64_t) { mle::v8w::Worker::current()->terminate(); };
  mle::common::Mailbox::open(config.spin, hooks, "mle_v8_worker");
  try {
    static mle::v8w::Worker worker(config);
    worker.run();
  } catch (const std::exception &e) {
    mle::common::Mailbox::failed_ready(std::string("V8 did not start: ") + e.what());
  }
}
