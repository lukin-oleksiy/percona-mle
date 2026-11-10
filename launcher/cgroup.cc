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

// cgroup v2: one leaf per worker below a delegated base directory.

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "launcher_internal.h"

namespace mle::launcher {

namespace {

constexpr long kCgroup2SuperMagic = 0x63677270;
/* Leaves are "mle-<launcher pid>-w<worker id>": several launchers (e.g.
   several mysqld on one host whose cgroups share a parent) can use one
   base without touching each other's leaves. */
constexpr const char *kLeafPrefix = "mle-";

/** Pid of the launcher that made a leaf, or 0 if the name is not a leaf. */
pid_t leaf_owner(const char *name) {
  if (strncmp(name, kLeafPrefix, strlen(kLeafPrefix)) != 0) return 0;
  char *end = nullptr;
  const long pid = strtol(name + strlen(kLeafPrefix), &end, 10);
  if (pid <= 0 || end == nullptr || strncmp(end, "-w", 2) != 0) return 0;
  return static_cast<pid_t>(pid);
}

bool write_file(const std::string &path, const std::string &value,
                std::string *error) {
  const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
  bool ok = fd >= 0 && write(fd, value.data(), value.size()) ==
                           static_cast<ssize_t>(value.size());
  if (!ok && error != nullptr)
    *error = "write " + path + ": " + strerror(errno);
  if (fd >= 0) close(fd);
  return ok;
}

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool has_word(const std::string &list, const std::string &word) {
  std::istringstream in(list);
  std::string w;
  while (in >> w)
    if (w == word) return true;
  return false;
}

}  // namespace

std::string cgroup_prepare(const std::string &base) {
  if (base.empty() || base[0] != '/')
    return "cgroup base must be an absolute path";
  struct statfs fs;
  if (statfs(base.c_str(), &fs) != 0) return base + ": " + strerror(errno);
  if (fs.f_type != kCgroup2SuperMagic)
    return base + " is not a cgroup v2 directory";
  const std::string controllers = read_file(base + "/cgroup.controllers");
  if (!has_word(controllers, "memory") || !has_word(controllers, "pids"))
    return base +
           ": the memory and pids controllers are not available "
           "(delegate them, e.g. systemd Delegate=yes)";
  // cgroup v2 "no internal processes": a cgroup that distributes controllers
  // to children must not hold processes itself.
  if (!read_file(base + "/cgroup.procs").empty())
    return base +
           " has processes of its own; move them to a child cgroup "
           "(e.g. systemd DelegateSubgroup=)";
  std::string error;
  if (!write_file(base + "/cgroup.subtree_control", "+memory +pids", &error))
    return error;
  // Leaves of launchers that are gone (their workers died with them; a
  // leaf that still has processes is not removed).
  if (DIR *d = opendir(base.c_str())) {
    while (dirent *e = readdir(d)) {
      const pid_t owner = leaf_owner(e->d_name);
      if (owner != 0 && owner != getpid() && kill(owner, 0) != 0 &&
          errno == ESRCH)
        rmdir((base + "/" + e->d_name).c_str());
    }
    closedir(d);
  }
  return "";
}

std::string cgroup_create_leaf(const std::string &base, uint64_t id,
                               const ClassConfig &c, std::string *error) {
  const std::string leaf = base + "/" + kLeafPrefix +
                           std::to_string(getpid()) + "-w" + std::to_string(id);
  if (mkdir(leaf.c_str(), 0755) != 0 && errno != EEXIST) {
    *error = "mkdir " + leaf + ": " + strerror(errno);
    return "";
  }
  const bool ok =
      (c.memory_max == 0 ||
       write_file(leaf + "/memory.max", std::to_string(c.memory_max), error)) &&
      write_file(leaf + "/memory.swap.max", "0", error) &&
      write_file(leaf + "/memory.oom.group", "1", error) &&
      (c.pids_max == 0 ||
       write_file(leaf + "/pids.max", std::to_string(c.pids_max), error));
  if (!ok) {
    rmdir(leaf.c_str());
    return "";
  }
  return leaf;
}

uint64_t cgroup_oom_kills(const std::string &leaf) {
  std::istringstream in(read_file(leaf + "/memory.events"));
  std::string key;
  uint64_t value;
  while (in >> key >> value)
    if (key == "oom_kill") return value;
  return 0;
}

bool cgroup_remove_leaf(const std::string &leaf) {
  if (leaf.empty() || rmdir(leaf.c_str()) == 0 || errno == ENOENT) return true;
  return false;
}

void cgroup_kill(const std::string &leaf) {
  write_file(leaf + "/cgroup.kill", "1", nullptr);
}

}  // namespace mle::launcher
