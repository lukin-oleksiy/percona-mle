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

#ifndef MLE_LAUNCHER_PROTOCOL_H
#define MLE_LAUNCHER_PROTOCOL_H

/**
  @file include/mle/launcher_protocol.h

  Messages between the MLE host (libmle_host) and mle_launcher over the
  SOCK_SEQPACKET socket that is the launcher's fd 3. One message per
  datagram, encoded with mle::Writer / mle::Reader (codec.h): u8 type, then
  the body below.

  host -> launcher
    kConfig    first message only. u32 protocol version, u32 arena size,
               str cgroup base (empty: no cgroup), u32 n + n x ClassConfig.
    kAcquire   u64 request id, u32 class index.
    kStats     u64 request id.
  launcher -> host
    kConfigReply  u8 ok, str error, u32 Landlock ABI (0: none),
                  u8 cgroup enabled, str cgroup info,
                  u32 n + n x str class error (empty: class usable).
    kWorker       u64 request id, u8 ok, str error, u64 worker id, u32 pid;
                  with ok, SCM_RIGHTS carries [mailbox memfd, host end of the
                  control socket, pidfd].
    kExited       u64 worker id, u32 wait status, u64 oom kills (from the
                  worker's cgroup memory.events). Sent for workers handed to
                  the host, at any time.
    kStatsReply   u64 request id, u32 n + n x ClassStats.

  ClassConfig = str name, str executable, u32 n + n x str argument (argv[1..]),
    u32 n + n x str environment entry, u32 n + n x str read-only path,
    u32 n + n x str read+execute path, u64 memory.max (0: no limit),
    u32 pids.max (0: no limit), u32 RLIMIT_NOFILE, u8 core dumps allowed,
    u32 pool size, u8 inherit stderr, u8 insecure without Landlock.
  ClassStats = u32 idle (in the pool), u64 spawned, u64 handed out,
    u64 pool deaths, u32 consecutive pool deaths, str last error.
*/

#include <cstdint>
#include <string>
#include <vector>

#include "mle/codec.h"

namespace mle::launcher {

constexpr uint32_t kVersion = 2;

/** Landlock ABI 6 (Linux 6.12; the POC's minimum is Linux 7.0) scopes
    signals and abstract unix sockets to the worker: below it a worker
    running as the mysqld OS user could signal mysqld (fcntl F_SETOWN +
    O_ASYNC is not a kill-family call the seccomp filter sees), and below 3
    truncate data files. */
constexpr int kMinLandlockAbi = 6;

/** After this many workers in a row died while idle in the pool, the
    launcher stops refilling that class's pool until a worker is handed out
    alive again. */
constexpr uint32_t kMaxPoolDeaths = 3;

enum class Msg : uint8_t {
  kConfig = 1,
  kAcquire = 2,
  kStats = 3,
  kConfigReply = 101,
  kWorker = 102,
  kExited = 103,
  kStatsReply = 104,
};

struct ClassConfig {
  std::string name;
  std::string executable;
  std::vector<std::string> args;
  std::vector<std::string> env;
  std::vector<std::string> read_paths;
  std::vector<std::string> exec_paths;
  /** Never readable, even below an allowed path (mysqld's plugin_dir with
      the keyring components' configuration, its datadir). */
  std::vector<std::string> hidden_paths;
  uint64_t memory_max = 0;
  uint32_t pids_max = 0;
  uint32_t nofile = 4096;
  bool core_dumps = false;
  uint32_t pool_size = 0;
  bool inherit_stderr = false;
  /** Tests only: run the class without Landlock when it is not available. */
  bool insecure_without_landlock = false;
};

struct ClassStats {
  uint32_t idle = 0;
  uint64_t spawned = 0;
  uint64_t handed_out = 0;
  uint64_t pool_deaths = 0;
  uint32_t consecutive_pool_deaths = 0;
  std::string last_error;
};

void put(Writer &w, const ClassConfig &c);
bool get(Reader &r, ClassConfig *c);
void put(Writer &w, const ClassStats &s);
bool get(Reader &r, ClassStats *s);
void put(Writer &w, const std::vector<std::string> &list);
bool get(Reader &r, std::vector<std::string> *list);

}  // namespace mle::launcher

#endif  // MLE_LAUNCHER_PROTOCOL_H
