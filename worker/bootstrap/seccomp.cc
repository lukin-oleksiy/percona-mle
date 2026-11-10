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

// The worker's seccomp filter. A deny list: the JVM and GraalVM need a wide
// and version-dependent set of system calls (memory, threads, futexes,
// signals, files), so we deny what a worker never needs and what would
// let it leave its box: new processes, exec, sockets, ptrace and other
// processes' memory, signals to other processes, namespaces and mounts,
// kernel keys, BPF, io_uring, userfaultfd, perf events, modules, reboot.

#include "bootstrap.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mle::worker {

namespace {

#if defined(__x86_64__)
constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
#error "MLE worker seccomp filter: unsupported architecture"
#endif

constexpr uint32_t kNr = offsetof(seccomp_data, nr);
constexpr uint32_t kArchOff = offsetof(seccomp_data, arch);
constexpr uint32_t kArg0Lo = offsetof(seccomp_data, args[0]);
constexpr uint32_t kArg0Hi = offsetof(seccomp_data, args[0]) + 4;

class Filter {
 public:
  void stmt(uint16_t code, uint32_t k) { m_f.push_back(BPF_STMT(code, k)); }
  void jump(uint16_t code, uint32_t k, uint8_t jt, uint8_t jf) {
    m_f.push_back(BPF_JUMP(code, k, jt, jf));
  }
  /** nr -> errno. */
  void deny(long nr, int err = EPERM) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (err & SECCOMP_RET_DATA));
  }
  /** nr allowed only when its first argument is pid; otherwise EPERM. */
  void only_self(long nr, pid_t pid) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 6);
    stmt(BPF_LD | BPF_W | BPF_ABS, kArg0Lo);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(pid), 0, 3);
    stmt(BPF_LD | BPF_W | BPF_ABS, kArg0Hi);
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  /** clone: threads only (CLONE_THREAD), no new processes. */
  void threads_only(long nr) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 4);
    stmt(BPF_LD | BPF_W | BPF_ABS, kArg0Lo);
    jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  std::vector<sock_filter> &code() { return m_f; }

 private:
  std::vector<sock_filter> m_f;
};

}  // namespace

int install_seccomp() {
  const pid_t self = getpid();
  Filter f;
  // Wrong architecture (e.g. 32-bit system calls): kill.
  f.stmt(BPF_LD | BPF_W | BPF_ABS, kArchOff);
  f.jump(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0);
  f.stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#if defined(__x86_64__)
  // x32 system calls share the architecture value; refuse them.
  f.stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
  f.jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1);
  f.stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif

  // Processes and programs.
  f.threads_only(__NR_clone);
  f.deny(__NR_clone3, ENOSYS);  // glibc then falls back to clone
#ifdef __NR_fork
  f.deny(__NR_fork);
#endif
#ifdef __NR_vfork
  f.deny(__NR_vfork);
#endif
  f.deny(__NR_execve);
  f.deny(__NR_execveat);
  // Signals: only to ourselves (the JVM signals its own threads).
  f.only_self(__NR_kill, self);
  f.only_self(__NR_tgkill, self);
  f.only_self(__NR_rt_sigqueueinfo, self);
  f.only_self(__NR_rt_tgsigqueueinfo, self);
  f.deny(__NR_tkill);
  f.deny(__NR_pidfd_open);
  f.deny(__NR_pidfd_send_signal);
  f.deny(__NR_pidfd_getfd);
  // Other processes' memory.
  f.deny(__NR_ptrace);
  f.deny(__NR_process_vm_readv);
  f.deny(__NR_process_vm_writev);
  f.deny(__NR_process_madvise);
  f.deny(__NR_kcmp);
  // Network and local sockets (the worker talks to the host only through
  // descriptors it already has).
  f.deny(__NR_socket);
  f.deny(__NR_socketpair);
  f.deny(__NR_connect);
  f.deny(__NR_bind);
  f.deny(__NR_listen);
  f.deny(__NR_accept);
  f.deny(__NR_accept4);
  // Namespaces, mounts, root.
  f.deny(__NR_unshare);
  f.deny(__NR_setns);
  f.deny(__NR_mount);
  f.deny(__NR_umount2);
  f.deny(__NR_pivot_root);
  f.deny(__NR_chroot);
  f.deny(__NR_open_tree);
  f.deny(__NR_move_mount);
  f.deny(__NR_fsopen);
  f.deny(__NR_fsmount);
  // Kernel attack surface a worker does not need.
  f.deny(__NR_bpf);
  f.deny(__NR_perf_event_open);
  f.deny(__NR_userfaultfd);
  f.deny(__NR_io_uring_setup);
  f.deny(__NR_io_uring_enter);
  f.deny(__NR_io_uring_register);
  f.deny(__NR_keyctl);
  f.deny(__NR_add_key);
  f.deny(__NR_request_key);
  f.deny(__NR_init_module);
  f.deny(__NR_finit_module);
  f.deny(__NR_delete_module);
  f.deny(__NR_kexec_load);
#ifdef __NR_kexec_file_load
  f.deny(__NR_kexec_file_load);
#endif
  f.deny(__NR_reboot);
  f.deny(__NR_swapon);
  f.deny(__NR_swapoff);
  f.deny(__NR_acct);
  f.deny(__NR_personality);
  f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

  sock_fprog prog{static_cast<unsigned short>(f.code().size()),
                  f.code().data()};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return errno;
  if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0)
    return errno;
  return 0;
}

}  // namespace mle::worker
