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

#ifndef MLE_ESCAPE_PROBES_H
#define MLE_ESCAPE_PROBES_H

// Things a hostile worker would try. Run by the MLE test worker under the
// launcher's lockdown (expected: denied) and by the test itself without it
// (expected: allowed), so that every probe is known to be real.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <string>

namespace mle_test {

struct ProbeArgs {
  pid_t host_pid;
  std::string secret_file;    // outside every allowed path
  std::string scratch_dir;    // outside every allowed path
  std::string allowed_file;   // below an allowed read path
  int closed_tcp_port;        // nothing listens there
  std::string abstract_name;  // abstract unix socket the host listens on
};

inline std::string result(int rc) {
  if (rc == 0) return "OK";
  const char *name = strerrorname_np(errno);
  return name != nullptr ? name : std::to_string(errno);
}

inline std::string open_probe(const std::string &path, int flags) {
  const int fd = open(path.c_str(), flags | O_CLOEXEC, 0600);
  const std::string r = result(fd < 0 ? -1 : 0);
  if (fd >= 0) close(fd);
  return r;
}

/** probe name -> "OK" or the errno name. */
inline std::map<std::string, std::string> run_escape_probes(
    const ProbeArgs &a) {
  std::map<std::string, std::string> r;
  r["read_secret"] = open_probe(a.secret_file, O_RDONLY);
  r["truncate_secret"] = result(truncate(a.secret_file.c_str(), 0));
  r["read_passwd"] = open_probe("/etc/passwd", O_RDONLY);
  r["create_file"] = open_probe(a.scratch_dir + "/created", O_CREAT | O_WRONLY);
  r["read_allowed"] = open_probe(a.allowed_file, O_RDONLY);
  r["write_allowed"] = open_probe(a.allowed_file, O_WRONLY);
  r["host_environ"] =
      open_probe("/proc/" + std::to_string(a.host_pid) + "/environ", O_RDONLY);
  r["signal_host"] = result(kill(a.host_pid, 0));
  {
    // Reaching the port counts as allowed: ECONNREFUSED means the
    // connection attempt went out.
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(a.closed_tcp_port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc =
        s < 0 ? -1 : connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    if (rc < 0 && errno == ECONNREFUSED) rc = 0;
    r["tcp_connect"] = result(rc);
    if (s >= 0) close(s);
  }
  {
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path + 1, a.abstract_name.data(), a.abstract_name.size());
    const socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + a.abstract_name.size());
    r["abstract_unix"] =
        result(s < 0 ? -1 : connect(s, reinterpret_cast<sockaddr *>(&sa), len));
    if (s >= 0) close(s);
  }
  {
    // exec of a program outside the allowed paths; the child reports the
    // execve errno as its exit status.
    const pid_t pid = fork();
    if (pid == 0) {
      char *argv[] = {const_cast<char *>("/usr/bin/true"), nullptr};
      char *envp[] = {nullptr};
      execve(argv[0], argv, envp);
      _exit(errno);
    }
    int status = 0;
    if (pid < 0) {
      r["exec_program"] = result(-1);
    } else {
      waitpid(pid, &status, 0);
      errno = WIFEXITED(status) ? WEXITSTATUS(status) : EINTR;
      r["exec_program"] = result(errno == 0 ? 0 : -1);
    }
  }
  return r;
}

inline std::string format_probes(const std::map<std::string, std::string> &r) {
  std::string s;
  for (const auto &[k, v] : r) s += k + "=" + v + "\n";
  return s;
}

}  // namespace mle_test

#endif  // MLE_ESCAPE_PROBES_H
