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

// A tiny MLE worker for the tests of the launcher and of the host's side of
// the protocol (no language runtime). It maps the mailbox and then behaves
// as the mode in argv[1] says:
//   ready             READY with a report of the process environment in
//                     its info, wait for EOF
//   escape ARGS       run the escape probes, report them, wait for EOF
//   exit N            exit with status N at once (no READY)
//   segv              crash at once (no READY)
//   oom               READY, then allocate memory until killed
//   ignore_eof        READY, then never exit on its own
//   fork N            fork N children that sleep, report how many worked
//   proto [SCRIPT]    READY (or SCRIPT in its place), then answer requests
//                     as a conforming worker, or as the scripts the test
//                     sends say (proto_script.h)

#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "escape_probes.h"
#include "mle/codec.h"
#include "mle/protocol.h"
#include "proto_script.h"

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
  m.info = info;
  m.worker_class = "test";
  m.languages = {"none"};
  m.runtime = "mle_test_worker";
  std::string body;
  encode(m, &body);
  if (body.size() > wire::kMaxSmallMessage) _exit(92);
  // At most 40 bytes in the worker line, else in the arena (§5.1).
  memcpy(wire::is_inline(body.size())
             ? reinterpret_cast<char *>(h->worker.body)
             : reinterpret_cast<char *>(h) + wire::kHeaderSize,
         body.data(), body.size());
  wire::MessageHeader hdr{};
  hdr.op = static_cast<uint32_t>(wire::Op::kReady);
  hdr.len = static_cast<uint32_t>(body.size());
  hdr.total_len = body.size();
  wire::store_header(h->worker, hdr);
  h->worker.word.store(1, std::memory_order_seq_cst);
  if (h->h_sleep.load(std::memory_order_seq_cst) != 0)
    syscall(SYS_futex, reinterpret_cast<uint32_t *>(&h->worker.word),
            FUTEX_WAKE, 1, nullptr, nullptr, 0);
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

/** The "proto" mode: a worker that keeps the protocol unless a script
    tells it otherwise. */
class Proto {
 public:
  explicit Proto(wire::RegionHeader *h)
      : m_h(h),
        m_arena(reinterpret_cast<char *>(h) + wire::kHeaderSize),
        m_arena_size(h->arena_size) {}

  /** READY, or the script in its place; then requests for ever. */
  [[noreturn]] void run(const std::string &ready_script) {
    if (ready_script.empty()) {
      ReadyMsg m;
      m.info = "proto";
      m.worker_class = "test";
      m.languages = {"none"};
      m.runtime = "mle_test_worker";
      std::string body;
      encode(m, &body);
      send(wire::Op::kReady, 0, 0, 0, body, 0);
    } else {
      Request ready;
      run_script(ready_script, ready);
    }
    while (true) {
      std::string body;
      const wire::MessageHeader h = receive(&body);
      serve(h, std::move(body));
    }
  }

 private:
  struct Routine {
    bool has_return = false;
    std::vector<uint32_t> outs;
    std::string call_script;
  };
  /** The request a script runs for. */
  struct Request {
    wire::MessageHeader h{};
    std::string body;
    Routine routine;
  };

  /** Posts m with bytes where the host looks for them by total_len (§5.1):
      inline (cut to 40 bytes) or in the arena (cut to its size). */
  void post(const wire::MessageHeader &m, std::string_view bytes) {
    if (wire::is_inline(m.total_len))
      memcpy(m_h->worker.body, bytes.data(),
             std::min<size_t>(bytes.size(), wire::kInlineBody));
    else
      memcpy(m_arena, bytes.data(),
             std::min<size_t>(bytes.size(), m_arena_size));
    wire::store_header(m_h->worker, m);
    m_h->worker.word.store(++m_posted, std::memory_order_seq_cst);
    if (m_h->h_sleep.load(std::memory_order_seq_cst) != 0)
      syscall(SYS_futex, reinterpret_cast<uint32_t *>(&m_h->worker.word),
              FUTEX_WAKE, 1, nullptr, nullptr, 0);
  }

  /** Where the body of the host's message with header m is (§5.1). */
  const char *host_body(const wire::MessageHeader &m) const {
    return wire::is_inline(m.total_len)
               ? reinterpret_cast<const char *>(m_h->host.body)
               : m_arena;
  }

  /** The sleep handshake of the specification, §4.3. */
  void wait_host() {
    while (true) {
      m_h->w_sleep.store(1, std::memory_order_seq_cst);
      if (m_h->host.word.load(std::memory_order_seq_cst) == m_seen)
        syscall(SYS_futex, reinterpret_cast<uint32_t *>(&m_h->host.word),
                FUTEX_WAIT, m_seen, nullptr, nullptr, 0);
      m_h->w_sleep.store(0, std::memory_order_release);
      const uint32_t v = m_h->host.word.load(std::memory_order_acquire);
      if (v != m_seen) {
        m_seen = v;
        return;
      }
    }
  }

  wire::MessageHeader header(wire::Op op, uint32_t status, uint32_t depth,
                             uint64_t seq) {
    wire::MessageHeader m{};
    m.op = static_cast<uint32_t>(op);
    m.status = status;
    m.depth = depth;
    m.call_seq = seq;
    return m;
  }

  /** A whole message of the host, its chunks answered with CONTINUE. The
      host is trusted, and this worker is a test of it: a chunk or a
      CONTINUE that breaks §5.2 ends the worker with status 98. */
  wire::MessageHeader receive(std::string *body) {
    wait_host();
    const wire::MessageHeader first = wire::load_header(m_h->host);
    wire::MessageHeader m = first;
    if (wire::is_inline(first.total_len) &&
        ((first.flags & wire::kMore) != 0 || first.len != first.total_len))
      _exit(98);
    body->assign(host_body(first), m.len);
    while (true) {
      const bool more = (m.flags & wire::kMore) != 0;
      if (m.op != first.op || m.status != 0 || m.depth != first.depth ||
          m.call_seq != first.call_seq || m.total_len != first.total_len ||
          (m.flags & ~wire::kKnownMessageFlags) != 0 ||
          (more ? m.len != m_arena_size || body->size() >= m.total_len
                : body->size() != m.total_len))
        _exit(98);
      if (!more) return first;
      if (auto it = m_armed.find(wire::Op::kContinue); it != m_armed.end()) {
        const std::string script = std::move(it->second);
        m_armed.erase(it);
        Request r;
        r.h = first;
        run_script(script, r);
      } else {
        post(header(wire::Op::kContinue, 0, first.depth, first.call_seq), {});
      }
      wait_host();
      m = wire::load_header(m_h->host);
      body->append(m_arena, m.len);
    }
  }

  void send(wire::Op op, uint32_t status, uint32_t depth, uint64_t seq,
            std::string_view body, uint32_t chunk_delay_ms) {
    wire::MessageHeader m = header(op, status, depth, seq);
    m.total_len = body.size();
    size_t sent = 0;
    while (true) {
      const size_t len = std::min<size_t>(m_arena_size, body.size() - sent);
      const bool more = sent + len < body.size();
      m.len = static_cast<uint32_t>(len);
      m.flags = more ? uint32_t{wire::kMore} : 0U;
      // A worker that asked to be retired says so in every RESULT, in all
      // its chunks.
      if (m_tainted && op == wire::Op::kResult) m.flags |= wire::kTainted;
      if (sent != 0 && chunk_delay_ms != 0) usleep(chunk_delay_ms * 1000);
      post(m, body.substr(sent, len));
      sent += len;
      if (!more) return;
      wait_host();
      const wire::MessageHeader go = wire::load_header(m_h->host);
      if (go.op != static_cast<uint32_t>(wire::Op::kContinue) ||
          go.status != 0 || go.flags != 0 || go.len != 0 ||
          go.total_len != 0 || go.depth != depth || go.call_seq != seq)
        _exit(98);
    }
  }

  /** A log entry: the header as on the wire (bytes 4 to 23 of a line,
      §5.1), then the body. */
  void log(const wire::MessageHeader &h, const std::string &body) {
    wire::Line line{};
    wire::store_header(line, h);
    m_log.append(reinterpret_cast<const char *>(&line) + 4, 20);
    m_log += body;
  }

  /** The host's reply to an SQL or LIBRARY message; nested requests that
      come before it are served. */
  void receive_reply() {
    while (true) {
      std::string body;
      const wire::MessageHeader h = receive(&body);
      const auto op = static_cast<wire::Op>(h.op);
      if (op != wire::Op::kPrepare && op != wire::Op::kCall &&
          op != wire::Op::kDrop) {
        if (m_logging) log(h, body);
        return;
      }
      serve(h, std::move(body));
    }
  }

  static std::string script_of(const std::string &text,
                               std::string_view prefix) {
    if (text.compare(0, prefix.size(), prefix) != 0) return {};
    return mle_test::unhex(std::string_view(text).substr(prefix.size()));
  }

  void serve(const wire::MessageHeader &h, std::string body) {
    Request r;
    r.h = h;
    r.body = std::move(body);
    const auto op = static_cast<wire::Op>(h.op);
    std::string script, error;
    switch (op) {
      case wire::Op::kPrepare: {
        PrepareMsg p;
        if (!decode(r.body, &p, &error)) _exit(95);
        Routine routine;
        routine.has_return = p.returns.has_value();
        for (uint32_t i = 0; i < p.params.size(); i++)
          if (p.params[i].mode != wire::ParamMode::kIn)
            routine.outs.push_back(i);
        routine.call_script = script_of(p.source, "call:");
        m_routines[p.routine_id] = std::move(routine);
        script = script_of(p.source, "prepare:");
        break;
      }
      case wire::Op::kCall: {
        CallMsg c;
        if (!decode(r.body, &c, &error)) _exit(95);
        const auto it = m_routines.find(c.routine_id);
        if (it == m_routines.end() || it->second.outs.size() > c.args.size())
          _exit(95);
        r.routine = it->second;
        script = r.routine.call_script;
        break;
      }
      case wire::Op::kParseLibrary: {
        ParseLibraryMsg p;
        if (!decode(r.body, &p, &error)) _exit(95);
        script = script_of(p.body, "script:");
        break;
      }
      case wire::Op::kDrop: {
        DropMsg d;
        if (!decode(r.body, &d, &error)) _exit(95);
        m_routines.erase(d.routine_id);
        break;
      }
      case wire::Op::kReset:
        m_routines.clear();
        break;
      case wire::Op::kOptions: {
        OptionsMsg o;
        if (!decode(r.body, &o, &error)) _exit(95);
        break;
      }
      default:
        _exit(96);  // not a request
    }
    if (auto it = m_armed.find(op); it != m_armed.end()) {
      script = std::move(it->second);
      m_armed.erase(it);
    }
    if (script.empty())
      answer(r);
    else
      run_script(script, r);
  }

  /** The RESULT of a conforming worker. */
  void answer(const Request &r) {
    ResultMsg m;
    const auto op = static_cast<wire::Op>(r.h.op);
    if (op == wire::Op::kCall) {
      if (r.routine.has_return) m.ret = Value::i64(42);
      for (const uint32_t index : r.routine.outs)
        m.out_params.emplace_back(index, Value::i64(index));
    }
    if (op == wire::Op::kCall || op == wire::Op::kPrepare) {
      m.console_out = std::move(m_log);
      m_log.clear();
      m_logging = false;
    }
    std::string body;
    encode(m, &body);
    send(wire::Op::kResult, 0, r.h.depth, r.h.call_seq, body, 0);
  }

  void await_interrupt(uint64_t seq) {
    while (true) {
      unsigned char buf[32];
      const ssize_t n = recv(wire::kControlFd, buf, sizeof buf, 0);
      if (n == 0) _exit(0);  // the host is gone
      uint64_t got;
      if (n != static_cast<ssize_t>(wire::kInterruptSize) ||
          buf[0] != static_cast<unsigned char>(wire::Control::kInterrupt))
        continue;
      memcpy(&got, buf + 1, sizeof got);
      if (got != seq) continue;
      // In the log the datagram is an entry of its own, with op 0.
      if (m_logging) {
        wire::MessageHeader h{};
        h.total_len = static_cast<uint64_t>(n);
        log(h, std::string(reinterpret_cast<const char *>(buf), n));
      }
      return;
    }
  }

  void run_script(std::string_view script, const Request &r) {
    using mle_test::Step;
    Reader in(script);
    while (in.remaining() != 0) {
      uint8_t step;
      if (!in.u8(&step)) _exit(97);
      switch (static_cast<Step>(step)) {
        case Step::kPost: {
          wire::MessageHeader m{};
          uint32_t depth_delta;
          int64_t seq_delta;
          std::string arena;
          if (!in.u32(&m.op) || !in.u32(&m.status) || !in.u32(&m.flags) ||
              !in.u32(&m.len) || !in.u64(&m.total_len) ||
              !in.u32(&depth_delta) || !in.i64(&seq_delta) ||
              !in.bytes(&arena))
            _exit(97);
          if (m.len == mle_test::kAuto)
            m.len = static_cast<uint32_t>(arena.size());
          if (m.total_len == mle_test::kAuto) m.total_len = arena.size();
          m.depth = r.h.depth + depth_delta;
          m.call_seq = r.h.call_seq + static_cast<uint64_t>(seq_delta);
          post(m, arena);
          break;
        }
        case Step::kWait:
          wait_host();
          break;
        case Step::kMessage: {
          uint32_t op, status, delay;
          std::string body;
          if (!in.u32(&op) || !in.u32(&status) || !in.u32(&delay) ||
              !in.bytes(&body))
            _exit(97);
          send(static_cast<wire::Op>(op), status, r.h.depth, r.h.call_seq, body,
               delay);
          if (op != static_cast<uint32_t>(wire::Op::kResult) &&
              op != static_cast<uint32_t>(wire::Op::kReady))
            receive_reply();
          break;
        }
        case Step::kSleep: {
          uint32_t ms;
          if (!in.u32(&ms)) _exit(97);
          usleep(ms * 1000);
          break;
        }
        case Step::kAwaitInterrupt:
          await_interrupt(r.h.call_seq);
          break;
        case Step::kExit: {
          uint32_t status;
          if (!in.u32(&status)) _exit(97);
          _exit(static_cast<int>(status));
        }
        case Step::kTainted:
          m_tainted = true;
          break;
        case Step::kBadStats:
          m_h->stats.heap_used = ~0ULL;
          m_h->stats.heap_max = 100;
          m_h->stats.state = 77;
          break;
        case Step::kArm: {
          uint32_t op;
          std::string armed;
          if (!in.u32(&op) || !in.bytes(&armed)) _exit(97);
          m_armed[static_cast<wire::Op>(op)] = std::move(armed);
          break;
        }
        case Step::kResize:
          if (ftruncate(wire::kMailboxFd, 0) == 0) _exit(8);  // sealed
          break;
        case Step::kAnswer:
          answer(r);
          break;
        case Step::kLoop: {
          uint32_t count;
          std::string body;
          if (!in.u32(&count) || !in.bytes(&body)) _exit(97);
          for (uint32_t i = 0; i < count; i++) run_script(body, r);
          break;
        }
        case Step::kLog:
          m_logging = true;
          log(r.h, r.body);
          break;
        case Step::kDrainControl:
          while (true) {
            unsigned char buf[32];
            const ssize_t n =
                recv(wire::kControlFd, buf, sizeof buf, MSG_DONTWAIT);
            if (n <= 0) break;
            wire::MessageHeader h{};
            h.total_len = static_cast<uint64_t>(n);
            log(h, std::string(reinterpret_cast<const char *>(buf), n));
          }
          break;
        default:
          _exit(97);
      }
    }
  }

  wire::RegionHeader *m_h;
  char *m_arena;
  uint32_t m_arena_size;
  uint32_t m_posted = 0;
  uint32_t m_seen = 0;
  std::map<uint64_t, Routine> m_routines;
  std::map<wire::Op, std::string> m_armed;
  std::string m_log;
  bool m_logging = false;
  bool m_tainted = false;
};

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
  if (mode == "read") {
    // read <path>...: "<path>=OK" or "<path>=<errno name>" per path, after the lockdown.
    std::string info;
    for (int i = 2; i < argc; i++) {
      const int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
      info += std::string(argv[i]) + '=' + (fd >= 0 ? "OK" : errno == EACCES ? "EACCES" : strerrorname_np(errno)) + '\n';
      if (fd >= 0) close(fd);
    }
    post_ready(h, info);
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
  if (mode == "proto")
    Proto(h).run(argc > 2 ? mle_test::unhex(argv[2]) : std::string());
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
