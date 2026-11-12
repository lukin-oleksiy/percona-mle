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
  @file worker/bootstrap/main.cc

  mle_worker: the MLE worker process (JVM worker, D-10).

    mle_worker --jvm=<path to libjvm.so> [--stack=<bytes>] [--dumpable]
               <JVM option>...

  Started by mle_launcher, which has already applied the Landlock rules,
  the cgroup and the descriptors (3 = mailbox, 4 = control socket). Here:
  1. PR_SET_DUMPABLE 0 (no core dumps, no ptrace by other processes of the
     same user) unless --dumpable;
  2. the seccomp filter (seccomp.cc), before any Java or guest code runs;
  3. the JVM, created with JNI in a new thread with a known stack size
     (HotSpot does not handle stack overflows reliably on the primordial
     thread), running com.percona.mle.worker.Main.run().
  There is no exec after the lockdown: libjvm is loaded with dlopen.

  Exit codes: 0 normal; 20 bad arguments; 21 dumpable; 22 seccomp; 23 libjvm;
  24 JNI_CreateJavaVM; 25 Main.run threw; 26 thread.
*/

#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bootstrap.h"

namespace {

constexpr size_t kDefaultStack = 8u << 20;

struct Args {
  std::string libjvm;
  size_t stack = kDefaultStack;
  bool dumpable = false;
  std::vector<std::string> jvm_options;
};

struct Run {
  const Args *args;
  int exit_code = 0;
};

using CreateJavaVM = jint (*)(JavaVM **, void **, void *);

void *run_jvm(void *p) {
  Run &run = *static_cast<Run *>(p);
  void *lib = dlopen(run.args->libjvm.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) {
    fprintf(stderr, "mle_worker: %s\n", dlerror());
    run.exit_code = 23;
    return nullptr;
  }
  auto create = reinterpret_cast<CreateJavaVM>(dlsym(lib, "JNI_CreateJavaVM"));
  if (create == nullptr) {
    run.exit_code = 23;
    return nullptr;
  }
  std::vector<std::string> opts = run.args->jvm_options;
  std::vector<JavaVMOption> jo(opts.size());
  for (size_t i = 0; i < opts.size(); i++) jo[i].optionString = opts[i].data();
  JavaVMInitArgs init{};
  init.version = JNI_VERSION_21;
  init.nOptions = static_cast<jint>(jo.size());
  init.options = jo.data();
  init.ignoreUnrecognized = JNI_FALSE;
  JavaVM *vm;
  JNIEnv *env;
  if (create(&vm, reinterpret_cast<void **>(&env), &init) != JNI_OK) {
    run.exit_code = 24;
    return nullptr;
  }
  jclass main = env->FindClass("com/percona/mle/worker/Main");
  jmethodID m =
      main == nullptr ? nullptr : env->GetStaticMethodID(main, "run", "()V");
  if (m != nullptr) env->CallStaticVoidMethod(main, m);
  if (m == nullptr || env->ExceptionCheck()) {
    env->ExceptionDescribe();
    run.exit_code = 25;
  }
  // Main.run returns only on a fatal error; the worker normally ends by
  // Runtime.halt when the host closes the control socket.
  return nullptr;
}

bool parse(int argc, char **argv, Args *a) {
  for (int i = 1; i < argc; i++) {
    std::string s = argv[i];
    if (s.rfind("--jvm=", 0) == 0)
      a->libjvm = s.substr(6);
    else if (s.rfind("--stack=", 0) == 0)
      a->stack = std::strtoull(s.c_str() + 8, nullptr, 10);
    else if (s == "--dumpable")
      a->dumpable = true;
    else
      a->jvm_options.push_back(s);
  }
  return !a->libjvm.empty() && a->stack >= (256u << 10);
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse(argc, argv, &args)) {
    fprintf(stderr,
            "usage: mle_worker --jvm=<libjvm.so> [--stack=<bytes>] "
            "[--dumpable] <JVM option>...\n");
    return 20;
  }
  if (!args.dumpable && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) return 21;
  if (const int err = mle::worker::install_seccomp(); err != 0) {
    fprintf(stderr, "mle_worker: seccomp: %s\n", strerror(err));
    return 22;
  }
  Run run{&args};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, args.stack);
  pthread_t thread;
  if (pthread_create(&thread, &attr, run_jvm, &run) != 0) return 26;
  pthread_join(thread, nullptr);
  return run.exit_code;
}
