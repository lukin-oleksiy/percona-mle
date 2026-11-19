# Percona MLE — multi-language stored programs for Percona Server

Stored functions and procedures in **JavaScript** (Oracle MySQL compatible `LANGUAGE JAVASCRIPT`), **Python**
(with numpy) and **Java**, run by GraalVM in sandboxed worker processes outside the server.

```sql
CREATE FUNCTION similarity(a VECTOR(768), b VECTOR(768)) RETURNS DOUBLE DETERMINISTIC LANGUAGE PYTHON AS $$
    import numpy as np
    return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
$$;
```

A separate product: it builds against Percona Server and installs into it as a component (`component_mle`); the
server needs only its generic language router (`component_lang_router`, built with js_lang or
`-DWITH_LANG_ROUTER=ON`). Supported server lines: trunk-based releases (26.x), then 9.7.

*A proof of concept.* Licensed under the GNU General Public License version 2 (`LICENSE`), as Percona Server. Code
comments cite the project's design notes by name (e.g. `14-phase6-libraries.md`); those notes are not published.

## Layout

| Directory | What |
|---|---|
| `component/` | `component_mle`: the server side (language service, sessions, SQL for routines, UDFs, variables) |
| `host/`, `include/mle/` | the host library: wire protocol and codec, launcher and worker clients, the installed layout |
| `launcher/` | `mle_launcher`: starts and sandboxes workers (clone, Landlock, seccomp, cgroups) |
| `worker/` | `mle_worker` (a JNI launcher of the JVM) and `mle-worker.jar` (GraalJS, GraalPy, Java routines, the SQL API); Maven; the numpy venv; the AOT cache; `cpython/`: the CPython worker; `v8/`: the V8 worker; `common/`: their shared C++ |
| `tools/` | `mle_package` (the DBA's npm/PyPI/Maven tool), `mle_aot_cache` (makes the AOT cache at install) |
| `harness/` | a command-line driver for workers (development) |
| `unittest/` | GoogleTest unit tests (9 binaries) |
| `mysql-test/suite/component_mle` | the MTR suite |
| `perf/` | the performance regression test: two builds side by side |
| `docker/` | the server + MLE image, the DBA tools image, compose |
| `packaging/`, `cmake/packaging.cmake` | packages (CPack, deb and rpm) and their scripts |
| `cmake/server.cmake` | building against a configured Percona Server tree |

## Build

Needs: a configured and built Percona Server (with the language router and its unit tests), a GraalVM Community
JDK 25 (`MLE_JAVA_HOME`, default `$JAVA_HOME`), Maven, network access to Maven Central and the GraalPy wheel index.

    cmake -G Ninja -S percona-mle -B BUILD-mle \
          -DMYSQL_SOURCE_DIR=<server source> -DMYSQL_BUILD_DIR=<server build> [-DMLE_BUILD_JDK=ON]
    ninja -C BUILD-mle

**Workers by language name** (exp design note 63): every installed worker runs its languages at once. The JVM
worker runs `LANGUAGE JAVASCRIPT` (GraalJS), `PYTHON` (GraalPy) and `JAVA`; the CPython worker `LANGUAGE CPYTHON`;
the V8 worker `LANGUAGE JS` (the name js_lang uses: `lang_router.language_map` picks which component runs it). A
session that uses two kinds holds two workers. Each kind has its own pool and limits (`mle.pool_size`,
`mle.memory_max`, `mle.heap_max` for the JVM worker; `mle.cpython_*`, `mle.v8_*`) and its own privilege
(`CREATE_CPYTHON_ROUTINE`; `CREATE_JS_ROUTINE`, shared with js_lang).

**The CPython worker** (`-DMLE_CPYTHON=ON`, the default when Python 3.12 or later with its headers is found,
e.g. `python3-dev`): `mle_cpython_worker` embeds the distribution's CPython and runs `LANGUAGE CPYTHON`, with
the same launcher, sandbox, protocol and SQL API (exp design note 62). A worker takes about 15 MB (PSS; 25 MB
with numpy) instead of hundreds, starts in about 60 ms, and frees numpy memory at once. The build installs numpy
(`MLE_CPYTHON_NUMPY`, default `numpy==2.4.4`) from PyPI into `cpython/site-packages`. In a build tree the
worker's Python code (`worker/cpython/python`) is linked, not copied: an edit needs no build, the next worker
reads it. Not on CPython: Java and JavaScript routines, cross-language libraries and the GraalVM-only helpers
(`com.percona.mle.api`: Stats, Vectors, Frame).

The build type follows the server build's (a debug server gives a debug MLE). For development the build links
`component_mle.so` and `mle/` into the server build's `plugin_output_directory`
(`-DMLE_LINK_INTO_SERVER_BUILD=OFF` to skip).

## Test

    BUILD-mle/mtr --parallel=4                           # the MTR suite on the server build
    systemd-run --user --scope -q -p Delegate=yes BUILD-mle/runtime_output_directory/mle_codec-t   # each gtest

Performance, a change against the commit before it (release builds, a quiet machine, the CPU governor
`performance`; about 25 minutes for 5 rounds):

    git worktree add ../percona-mle-base <commit>        # the code to compare with, built once:
    cmake -S ../percona-mle-base -B ../BUILD-mle-base -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMYSQL_SOURCE_DIR=... -DMYSQL_BUILD_DIR=<release server build> -DMLE_BUILD_JDK=ON \
      -DMLE_LINK_INTO_SERVER_BUILD=OFF -DMLE_WITH_UNIT_TESTS=OFF && ninja -C ../BUILD-mle-base
    perf/regression.py --server <release server build> --base ../BUILD-mle-base --new ../BUILD-mle-release

It runs each case of `perf/setup.sql` (the call path, values of every kind, SQL from routines read and written,
libraries, errors, vectors, a new connection's worker) on both builds in alternating order on the same data, and
exits with status 1 when a case is slower beyond the threshold (5 %) in all rounds but one.

## Diagnostics

Environment variables of mysqld, for diagnosing and profiling workers; not for production:

| Variable | Effect |
|---|---|
| `MLE_WORKER_STDERR=1` | workers write to mysqld's stderr (the error log) |
| `MLE_JVM_OPTIONS="..."` | more JVM options for JVM workers |
| `MLE_DEBUG_WRITE_DIR=/dir` | workers may write files in `/dir` (the sandbox allows it; a warning in the error log) |

A JDK Flight Recorder recording of a JVM worker:

    MLE_DEBUG_WRITE_DIR=/tmp/mle-jfr \
    MLE_JVM_OPTIONS="-XX:FlightRecorderOptions=repository=/tmp/mle-jfr -XX:StartFlightRecording=delay=10s,duration=30s,filename=/tmp/mle-jfr/w.jfr,settings=profile" \
      mysqld ...
    jfr view hot-methods /tmp/mle-jfr/w.jfr

The same directory takes Truffle's compilation log: `-Dpolyglot.log.file=/dir/truffle.log
-Dpolyglot.engine.TraceCompilation=true`.

## Install and packages

Packages (`cpack -G DEB` or `cpack -G RPM` in the build directory; the server must be built with
`INSTALL_LAYOUT=DEB` or `RPM`), one per install component (`cmake --install BUILD-mle --component <c>`):

| Package | Component | Contents | Needs |
|---|---|---|---|
| `percona-mle-common` | `MleCommon` | `component_mle`, `mle_launcher`, `mle_worker` | the server |
| `percona-mle-apparmor` | `MleApparmor` | the AppArmor profile (DEB only; `-DMLE_APPARMOR=ON`, the default where `/etc/apparmor.d` exists) | common; recommended by it |
| `percona-mle` | `MleCore` | the JVM build: worker jar, runtime jars, JavaScript and Java | common, jdk |
| `percona-mle-jdk` | `MleJdk` | a trimmed GraalVM runtime (`-DMLE_BUILD_JDK=ON`) | |
| `percona-mle-python-runtime` | `MlePythonRuntime` | GraalPy's standard library and the numpy venv | common |
| `percona-mle-python` | `MlePython` | GraalPy for the JVM build | percona-mle, python-runtime |
| `percona-mle-cpython` | `MleCPython` | the CPython worker, its Python code and numpy (`-DMLE_CPYTHON=ON`) | common, the distribution's libpython3.x |
| `percona-mle-tools` | `MleTools` | `mle_package` (third-party npm, PyPI, Maven packages; `install cpython ...` for the CPython worker) | common |
| `percona-mle-selinux` | `MleSelinux` | the SELinux policy module (RPM only; `-DMLE_SELINUX=ON`, the default with selinux-policy-devel) | pulled by common where SELinux policy is installed |

With the JVM build, `<plugin dir>/mle/mle_aot_cache` makes the JDK AOT cache that workers start from (the
`percona-mle` package runs it: Debian triggers, RPM file triggers, once per transaction that changes the JDK,
jars or Python).

`mle_package install python` needs a GraalPy with pip, of the workers' GraalVM version (the version in the name of
`<plugin dir>/mle/deps/python-language-<version>.jar`), and the packages do not bring one: the venv they install is
for the workers only, without its `bin/`. Get a GraalPy of that version, make a venv with it
(`graalpy -m venv DIR`) and give `mle_package --graalpy DIR/bin/graalpy`; `docker/Dockerfile.tools` does this with
GraalPy's jars from Maven on MLE's own runtime. In a build tree the tool finds the build's venv itself.

SELinux (`packaging/selinux/percona_mle.te`): mysqld (`mysqld_t`) starts `mle_launcher` in `mle_launcher_t`
(cgroups, mailboxes; no JIT), which starts workers in `mle_worker_t` (`execmem` for the JIT compilers); mysqld
gains only the workers' descriptors and signals. On RHEL-like kernels Landlock must be enabled on the kernel
command line (`lsm=landlock,...`).

AppArmor (`packaging/apparmor`, Debian and Ubuntu): `percona-mle` is the profile of `mle_launcher` and the workers
(they start with `no_new_privs`, so they keep the launcher's profile); `abstractions/percona-mle-mysqld` holds the
rules mysqld's profile needs, and the package's `postinst` includes it from `local/usr.sbin.mysqld`. Both follow the
mode of the server's packaged profile (complain). Checked enforcing in an Ubuntu 26.04 VM with a server installed in
the package layout: JVM workers, cgroups, packages, `KILL`, a dying worker, no denials.

In the server:

    INSTALL COMPONENT 'file://component_lang_router';
    INSTALL COMPONENT 'file://component_mle';
    GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO ...;

Linux only: Landlock ABI ≥ 6 (kernel 6.12+), cgroup v2 with a delegated cgroup for per-worker memory limits.
x86-64 so far.
