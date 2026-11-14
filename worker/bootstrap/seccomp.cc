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
#include <sys/socket.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
constexpr uint32_t arg_lo(int i) { return offsetof(seccomp_data, args) + 8 * i; }
constexpr uint32_t arg_hi(int i) { return arg_lo(i) + 4; }

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
  /** nr allowed only when argument arg is 0 (this process or thread) or
      pid: a worker runs as mysqld's OS user, and these calls work on any
      process of that user (rlimits, scheduling, I/O priority). */
  void zero_or_self(long nr, int arg, pid_t pid) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 7);
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_hi(arg));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3);  // high word set: refuse
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(arg));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(pid), 1, 0);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  }
  /** nr allowed only for one process: argument 0 (`which`) is
      which_process (PRIO_PROCESS, IOPRIO_WHO_PROCESS) and argument 1
      (`who`) is 0 or pid. Other `which` values with who 0 mean the
      caller's process group or user: every process of mysqld's OS user. */
  void process_only(long nr, uint32_t which_process, pid_t pid) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 9);
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(0));
    jump(BPF_JMP | BPF_JEQ | BPF_K, which_process, 0, 5);
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_hi(1));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3);  // high word set: refuse
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(1));
    jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(pid), 1, 0);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  }
  /** nr refused (EPERM) when argument arg is one of values. */
  void deny_arg(long nr, int arg, std::initializer_list<uint32_t> values) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0,
         static_cast<uint8_t>(values.size() + 3));
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(arg));
    uint8_t left = static_cast<uint8_t>(values.size());
    for (uint32_t v : values) {
      --left;
      jump(BPF_JMP | BPF_JEQ | BPF_K, v, static_cast<uint8_t>(left + 1), 0);
    }
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  /** socketpair: AF_UNIX stream pairs only (a datagram socket can send to
      any pathname socket of the host without connect or bind). */
  void unix_stream_pairs(long nr) {
    stmt(BPF_LD | BPF_W | BPF_ABS, kNr);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 7);
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(0));
    jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 4);
    stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(1));
    stmt(BPF_ALU | BPF_AND | BPF_K, 0xf);  // the type without SOCK_ flags
    jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
  }
  /** nr allowed only when its first argument is value (e.g. a socket
      domain); otherwise EPERM. */
  void only_arg0(long nr, uint32_t value) { only_self(nr, static_cast<pid_t>(value)); }
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
  // AF_UNIX stream pairs only: two connected descriptors inside the process
  // (the JDK's NIO dispatcher makes one when it starts; GraalPy's os.pipe()
  // uses NIO). They reach no network and no other process.
  f.unix_stream_pairs(__NR_socketpair);
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
  // Other processes of mysqld's OS user (the worker runs as that user):
  // limits, scheduling, I/O priority, memory placement only for itself.
  f.zero_or_self(__NR_prlimit64, 0, self);
  f.zero_or_self(__NR_sched_setaffinity, 0, self);
  f.zero_or_self(__NR_sched_setscheduler, 0, self);
  f.zero_or_self(__NR_sched_setparam, 0, self);
  f.zero_or_self(__NR_sched_setattr, 0, self);
  // Priorities: PRIO_PROCESS / IOPRIO_WHO_PROCESS (0 / 1) only; the group
  // and user forms reach mysqld.
  f.process_only(__NR_setpriority, 0, self);
  f.process_only(__NR_ioprio_set, 1, self);
  f.deny(__NR_move_pages);
  f.deny(__NR_migrate_pages);
  // Files: Landlock does not cover mode, owner, times and extended
  // attributes, and mysqld's files have the worker's owner.
#ifdef __NR_chmod
  f.deny(__NR_chmod);
#endif
  f.deny(__NR_fchmod);
  f.deny(__NR_fchmodat);
#ifdef __NR_fchmodat2
  f.deny(__NR_fchmodat2);
#endif
#ifdef __NR_chown
  f.deny(__NR_chown);
#endif
#ifdef __NR_lchown
  f.deny(__NR_lchown);
#endif
  f.deny(__NR_fchown);
  f.deny(__NR_fchownat);
#ifdef __NR_utime
  f.deny(__NR_utime);
#endif
#ifdef __NR_utimes
  f.deny(__NR_utimes);
#endif
#ifdef __NR_futimesat
  f.deny(__NR_futimesat);
#endif
  f.deny(__NR_utimensat);
  f.deny(__NR_setxattr);
  f.deny(__NR_lsetxattr);
  f.deny(__NR_fsetxattr);
  f.deny(__NR_removexattr);
  f.deny(__NR_lremovexattr);
  f.deny(__NR_fremovexattr);
#ifdef __NR_setxattrat
  f.deny(__NR_setxattrat);
#endif
#ifdef __NR_removexattrat
  f.deny(__NR_removexattrat);
#endif
  // Watching the host's files, file handles, global syncs, the kernel log.
  f.deny(__NR_inotify_add_watch);
  f.deny(__NR_fanotify_init);
  f.deny(__NR_fanotify_mark);
  f.deny(__NR_name_to_handle_at);
  f.deny(__NR_open_by_handle_at);
  f.deny(__NR_sync);
  f.deny(__NR_syncfs);
  f.deny(__NR_syslog);
#ifdef __NR_modify_ldt
  f.deny(__NR_modify_ldt);
#endif
  // SysV IPC and POSIX message queues: objects that outlive the worker and
  // its cgroup, shared with every process of the OS user.
  f.deny(__NR_shmget);
  f.deny(__NR_shmat);
  f.deny(__NR_shmctl);
  f.deny(__NR_msgget);
  f.deny(__NR_msgsnd);
  f.deny(__NR_msgrcv);
  f.deny(__NR_msgctl);
  f.deny(__NR_semget);
  f.deny(__NR_semop);
  f.deny(__NR_semctl);
  f.deny(__NR_semtimedop);
  f.deny(__NR_mq_open);
  f.deny(__NR_mq_unlink);
  f.deny(__NR_mq_timedsend);
  f.deny(__NR_mq_timedreceive);
  f.deny(__NR_mq_notify);
  f.deny(__NR_mq_getsetattr);
  // prctl: the bootstrap's settings stay (not dumpable, dies with the
  // launcher, no ptracer exception).
  f.deny_arg(__NR_prctl, 0, {PR_SET_DUMPABLE, PR_SET_PDEATHSIG, PR_SET_PTRACER});
  f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

  sock_fprog prog{static_cast<unsigned short>(f.code().size()),
                  f.code().data()};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return errno;
  if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0)
    return errno;
  return 0;
}

}  // namespace mle::worker
