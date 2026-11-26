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

package com.percona.mle.worker;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

/** The few libc functions the worker needs, through FFM downcalls (restricted methods, by design). */
@SuppressWarnings("restricted")
final class Native {
  private Native() {}

  private static final Linker LINKER = Linker.nativeLinker();
  private static final SymbolLookup LIBC = LINKER.defaultLookup();

  private static MethodHandle fn(String name, FunctionDescriptor d, Linker.Option... options) {
    return LINKER.downcallHandle(LIBC.find(name).orElseThrow(), d, options);
  }

  private static final MethodHandle MMAP =
      fn(
          "mmap",
          FunctionDescriptor.of(
              ValueLayout.ADDRESS,
              ValueLayout.ADDRESS,
              ValueLayout.JAVA_LONG,
              ValueLayout.JAVA_INT,
              ValueLayout.JAVA_INT,
              ValueLayout.JAVA_INT,
              ValueLayout.JAVA_LONG));
  private static final MethodHandle MUNMAP =
      fn("munmap", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
  private static final MethodHandle MALLOC_TRIM =
      fn("malloc_trim", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG));

  /** glibc's malloc_trim(0): free memory at the top and inside malloc's heaps back to the system. */
  static void mallocTrim() {
    try {
      int unused = (int) MALLOC_TRIM.invokeExact(0L);
    } catch (Throwable t) {
      // Not glibc's malloc, or refused: nothing given back.
    }
  }

  // syscall(SYS_futex, addr, op, val, timeout, 0, 0): variadic in C.
  private static final MethodHandle SYSCALL_FUTEX =
      fn(
          "syscall",
          FunctionDescriptor.of(
              ValueLayout.JAVA_LONG,
              ValueLayout.JAVA_LONG,
              ValueLayout.ADDRESS,
              ValueLayout.JAVA_INT,
              ValueLayout.JAVA_INT,
              ValueLayout.ADDRESS,
              ValueLayout.ADDRESS,
              ValueLayout.JAVA_INT),
          Linker.Option.firstVariadicArg(1));
  /**
   * read(2) and lseek(2) share one descriptor, long f(int, long, long): read's buffer goes as its
   * address and lseek's whence as a long (LP64, arguments in registers: the callee reads an int's
   * low 32 bits). The Linker keeps one downcall stub per descriptor; each new one costs a stub and
   * some twenty LambdaForm classes made at run time, ~3.5 ms of every worker's start.
   */
  private static final FunctionDescriptor INT_LONG_LONG =
      FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG);

  private static final MethodHandle READ = fn("read", INT_LONG_LONG);
  private static final MethodHandle LSEEK = fn("lseek", INT_LONG_LONG);

  static final long SEEK_END = 2;
  static final int PROT_READ = 1;
  static final int PROT_WRITE = 2;
  static final int MAP_SHARED = 1;
  static final int FUTEX_WAIT = 0;
  static final int FUTEX_WAKE = 1;

  private static final long SYS_FUTEX =
      switch (System.getProperty("os.arch")) {
        case "amd64", "x86_64" -> 202;
        case "aarch64" -> 98;
        default -> throw new ExceptionInInitializerError("unsupported architecture");
      };

  /** mmap(NULL, size, prot, MAP_SHARED, fd, 0); a segment of size bytes. */
  static MemorySegment mapShared(int fd, long size) {
    try {
      MemorySegment p =
          (MemorySegment) MMAP.invokeExact(MemorySegment.NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0L);
      if (p.address() == -1L) throw new IllegalStateException("mmap failed");
      return p.reinterpret(size);
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  /** The size of the file behind fd (lseek to its end); -1 on error. */
  static long fileSize(int fd) {
    try {
      return (long) LSEEK.invokeExact(fd, 0L, SEEK_END);
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  static void unmap(MemorySegment s) {
    try {
      int unused = (int) MUNMAP.invokeExact(s, s.byteSize());
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  /** FUTEX_WAIT on a shared (not process-private) futex word: sleeps while *word == expected. */
  static void futexWait(MemorySegment word, int expected) {
    try {
      long unused =
          (long) SYSCALL_FUTEX.invokeExact(SYS_FUTEX, word, FUTEX_WAIT, expected, MemorySegment.NULL, MemorySegment.NULL, 0);
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  /** The timeout of futexWait(word, expected, nanos): a struct timespec, of the dispatcher's thread only. */
  private static final MemorySegment TIMEOUT = java.lang.foreign.Arena.global().allocate(16, 8);

  /** As futexWait, for at most nanos (relative): a timeout, a wake-up or a changed word return. */
  static void futexWait(MemorySegment word, int expected, long nanos) {
    TIMEOUT.set(ValueLayout.JAVA_LONG, 0, nanos / 1_000_000_000L);
    TIMEOUT.set(ValueLayout.JAVA_LONG, 8, nanos % 1_000_000_000L);
    try {
      long unused = (long) SYSCALL_FUTEX.invokeExact(SYS_FUTEX, word, FUTEX_WAIT, expected, TIMEOUT, MemorySegment.NULL, 0);
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  static void futexWake(MemorySegment word) {
    try {
      long unused = (long) SYSCALL_FUTEX.invokeExact(SYS_FUTEX, word, FUTEX_WAKE, 1, MemorySegment.NULL, MemorySegment.NULL, 0);
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  /** read(2) into a native segment; -1 on error, 0 on EOF. */
  static long read(int fd, MemorySegment buf) {
    try {
      return (long) READ.invokeExact(fd, buf.address(), buf.byteSize());
    } catch (Throwable t) {
      throw rethrow(t);
    }
  }

  private static RuntimeException rethrow(Throwable t) {
    if (t instanceof RuntimeException r) return r;
    if (t instanceof Error e) throw e;
    return new IllegalStateException(t);
  }
}
