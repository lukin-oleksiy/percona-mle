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

// A tiny MLE worker for the launcher tests (no GraalVM). It maps the
// mailbox, posts READY with a report in ReadyMsg::info, and then behaves as
// the mode in argv[1] says:
//   ready             report the process environment, wait for EOF
//   escape ARGS       run the escape probes, report them, wait for EOF
//   exit N            exit with status N at once (no READY)
//   segv              crash at once (no READY)
//   oom               READY, then allocate memory until killed
//   ignore_eof        READY, then never exit on its own
//   fork N            fork N children that sleep, report how many worked
//   proto BEHAVIOUR   READY, then answer requests, well (good) or badly
//                     (see proto_loop), for the host's protocol checks

#include <dirent.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "escape_probes.h"
#include "mle/codec.h"
#include "mle/protocol.h"

namespace {

using namespace mle;

wire::RegionHeader *map_mailbox() {
  void *p = mmap(nullptr, wire::kHeaderSize, PROT_READ, MAP_SHARED,
                 wire::kMailboxFd, 0);
  if (p == MAP_FAILED) _exit(90);
  const auto size = static_cast<const wire::RegionHeader *>(p)->region_size;
  munmap(p, wire::kHeaderSize);
  p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, wire::kMailboxFd,
           0);
  if (p == MAP_FAILED) _exit(91);
  return static_cast<wire::RegionHeader *>(p);
}

void post_ready(wire::RegionHeader *h, const std::string &info) {
  ReadyMsg m;
  m.worker_class = "test";
  m.languages = {"none"};
  m.runtime = "mle_test_worker";
  m.info = info;
  std::string body;
  encode(m, &body);
  if (body.size() > h->arena_size) _exit(92);
  memcpy(reinterpret_cast<char *>(h) + wire::kHeaderSize, body.data(),
         body.size());
  h->msg = wire::MessageHeader{};
  h->msg.op = static_cast<uint32_t>(wire::Op::kReady);
  h->msg.len = static_cast<uint32_t>(body.size());
  h->msg.total_len = body.size();
  h->to_h.fetch_add(1, std::memory_order_seq_cst);
  if (h->h_sleep.load(std::memory_order_seq_cst) != 0)
    syscall(SYS_futex, reinterpret_cast<uint32_t *>(&h->to_h), FUTEX_WAKE, 1,
            nullptr, nullptr, 0);
}

[[noreturn]] void wait_for_eof() {
  char c;
  while (read(wire::kControlFd, &c, 1) != 0) {
  }
  _exit(0);
}

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string status_field(const std::string &name) {
  std::istringstream in(read_file("/proc/self/status"));
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(name + ":", 0) == 0) {
      std::string v = line.substr(name.size() + 1);
      v.erase(0, v.find_first_not_of(" \t"));
      return v;
    }
  return "?";
}

std::string environment_report() {
  std::string r;
  // Open descriptors.
  std::vector<int> fds;
  if (DIR *d = opendir("/proc/self/fd")) {
    const int own = dirfd(d);
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.' && atoi(e->d_name) != own)
        fds.push_back(atoi(e->d_name));
    closedir(d);
  }
  std::sort(fds.begin(), fds.end());
  r += "fds=";
  for (size_t i = 0; i < fds.size(); i++)
    r += (i ? "," : "") + std::to_string(fds[i]);
  r += "\n";
  char cwd[PATH_MAX] = "?";
  if (getcwd(cwd, sizeof cwd) == nullptr) strcpy(cwd, "?");
  r += std::string("cwd=") + cwd + "\n";
  r += "env=";
  for (char **e = environ; *e != nullptr; e++) r += std::string(*e) + ";";
  r += "\n";
  rlimit lim;
  getrlimit(RLIMIT_CORE, &lim);
  r += "core=" + std::to_string(lim.rlim_cur) + "\n";
  getrlimit(RLIMIT_NOFILE, &lim);
  r += "nofile=" + std::to_string(lim.rlim_cur) + "\n";
  r += "nnp=" + status_field("NoNewPrivs") + "\n";
  int sig = 0;
  prctl(PR_GET_PDEATHSIG, &sig);
  r += "pdeathsig=" + std::to_string(sig) + "\n";
  r += "seal=" + mle_test::result(ftruncate(wire::kMailboxFd, 0)) + "\n";
  char target[PATH_MAX] = "";
  const ssize_t n = readlink("/proc/self/fd/0", target, sizeof target - 1);
  r +=
      "stdin=" + std::string(target, n > 0 ? static_cast<size_t>(n) : 0) + "\n";
  std::string cg = read_file("/proc/self/cgroup");
  if (!cg.empty() && cg.back() == '\n') cg.pop_back();
  r += "cgroup=" + cg + "\n";
  r += "ppid=" + std::to_string(getppid()) + "\n";
  return r;
}

void post_msg(wire::RegionHeader *h, wire::Op op, uint32_t status,
              uint32_t flags, uint32_t depth, uint32_t len, uint64_t total,
              uint64_t seq, const std::string &body) {
  memcpy(reinterpret_cast<char *>(h) + wire::kHeaderSize, body.data(),
         std::min<size_t>(body.size(), h->arena_size));
  h->msg = wire::MessageHeader{};
  h->msg.op = static_cast<uint32_t>(op);
  h->msg.status = status;
  h->msg.flags = flags;
  h->msg.depth = depth;
  h->msg.len = len;
  h->msg.total_len = total;
  h->msg.call_seq = seq;
  h->to_h.fetch_add(1, std::memory_order_seq_cst);
  if (h->h_sleep.load(std::memory_order_seq_cst) != 0)
    syscall(SYS_futex, reinterpret_cast<uint32_t *>(&h->to_h), FUTEX_WAKE, 1,
            nullptr, nullptr, 0);
}

/** Answers host requests. behaviour "good" answers correctly; every other
    behaviour breaks one rule, once, at the first CALL (or PREPARE where
    noted). */
[[noreturn]] void proto_loop(wire::RegionHeader *h, const std::string &how) {
  if (how == "silent") signal(SIGTERM, SIG_IGN);
  uint32_t seen = 0;
  while (true) {
    while (h->to_w.load(std::memory_order_acquire) == seen) usleep(10);
    seen = h->to_w.load(std::memory_order_acquire);
    const wire::MessageHeader m = h->msg;
    const bool call = m.op == static_cast<uint32_t>(wire::Op::kCall);
    ResultMsg good;
    if (call) good.ret = Value::i64(42);
    std::string body;
    encode(good, &body);
    const auto len = static_cast<uint32_t>(body.size());
    auto answer = [&](const std::string &b, uint32_t status = 0,
                      uint32_t depth_delta = 0, uint64_t seq_delta = 0) {
      post_msg(h, wire::Op::kResult, status, 0, m.depth + depth_delta,
               static_cast<uint32_t>(b.size()), b.size(),
               m.call_seq + seq_delta, b);
    };
    if (how == "sql_in_prepare" &&
        m.op == static_cast<uint32_t>(wire::Op::kPrepare)) {
      SqlMsg sql;
      sql.statement = "SELECT 1";
      std::string b;
      encode(sql, &b);
      post_msg(h, wire::Op::kSql, 0, 0, m.depth,
               static_cast<uint32_t>(b.size()), b.size(), m.call_seq, b);
      continue;
    }
    if (!call || how == "good") {
      answer(body);
      continue;
    }
    if (how == "bad_op") {
      post_msg(h, static_cast<wire::Op>(99), 0, 0, m.depth, len, len,
               m.call_seq, body);
    } else if (how == "big_len") {
      post_msg(h, wire::Op::kResult, 0, 0, m.depth, h->arena_size + 1,
               h->arena_size + 1ULL, m.call_seq, body);
    } else if (how == "bad_body") {
      answer(std::string("\x05", 1));
    } else if (how == "wrong_depth") {
      answer(body, 0, 5);
    } else if (how == "wrong_seq") {
      answer(body, 0, 0, 1);
    } else if (how == "status_without_error") {
      answer(body, static_cast<uint32_t>(wire::Status::kGuestError));
    } else if (how == "out_for_in") {
      // An OUT value for a parameter that is not OUT or INOUT.
      ResultMsg r = good;
      r.out_params = {{0, Value::i64(1)}};
      std::string b;
      encode(r, &b);
      answer(b);
    } else if (how == "missing_return") {
      ResultMsg r;
      std::string b;
      encode(r, &b);
      answer(b);
    } else if (how == "host_status") {
      answer(body, static_cast<uint32_t>(wire::Status::kKilled));
    } else if (how == "chunk_lie") {
      post_msg(h, wire::Op::kResult, 0, wire::kMore, m.depth, len, len,
               m.call_seq, body);
    } else if (how == "die") {
      _exit(7);
    } else if (how == "silent") {
      // never answers
    } else if (how == "bad_stats") {
      h->stats.heap_used = ~0ULL;
      h->stats.heap_max = 100;
      h->stats.state = 77;
      answer(body);
    } else if (how == "tainted") {
      h->stats.tainted = 1;
      answer(body);
    } else if (how == "resize_mailbox") {
      const int rc = ftruncate(wire::kMailboxFd, 0);  // sealed: must fail
      if (rc == 0) _exit(8);
      answer(body);
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "ready";
  if (mode == "exit") _exit(argc > 2 ? atoi(argv[2]) : 0);
  if (mode == "segv") raise(SIGSEGV);

  wire::RegionHeader *h = map_mailbox();
  if (mode == "ready") {
    post_ready(h, environment_report());
    wait_for_eof();
  }
  if (mode == "escape" && argc == 8) {
    mle_test::ProbeArgs a{static_cast<pid_t>(atoi(argv[2])),
                          argv[3],
                          argv[4],
                          argv[5],
                          atoi(argv[6]),
                          argv[7]};
    post_ready(h, mle_test::format_probes(mle_test::run_escape_probes(a)));
    wait_for_eof();
  }
  if (mode == "oom") {
    post_ready(h, "allocating");
    std::vector<char *> blocks;
    while (true) {
      char *b = static_cast<char *>(malloc(1 << 20));
      if (b == nullptr) _exit(93);
      memset(b, 1, 1 << 20);  // touch it, so it counts
      blocks.push_back(b);
    }
  }
  if (mode == "ignore_eof") {
    signal(SIGTERM, SIG_IGN);
    post_ready(h, "ignoring EOF");
    while (true) pause();
  }
  if (mode == "proto" && argc == 3) {
    post_ready(h, "proto");
    proto_loop(h, argv[2]);
  }
  if (mode == "fork" && argc == 3) {
    int ok = 0;
    for (int i = 0; i < atoi(argv[2]); i++) {
      const pid_t pid = fork();
      if (pid == 0) {
        while (true) pause();
      }
      if (pid > 0) ok++;
    }
    post_ready(h, "forked=" + std::to_string(ok));
    wait_for_eof();
  }
  return 94;  // unknown mode
}
