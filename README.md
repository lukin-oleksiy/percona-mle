# Percona MLE — multi-language stored programs for Percona Server

Stored functions and procedures in **JavaScript**, **Python** (with numpy) and **Java**, run in sandboxed worker
processes outside the server, with SQL from routines in every language.

```sql
CREATE FUNCTION similarity(a VECTOR(768), b VECTOR(768)) RETURNS DOUBLE DETERMINISTIC LANGUAGE PYTHON AS $$
    import numpy as np
    return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
$$;
```

Three kinds of worker, one package each; they can be installed together, and a routine's `LANGUAGE` chooses the
worker:

| Worker | `LANGUAGE` | Runtime | For |
|---|---|---|---|
| JVM worker | `JAVASCRIPT` (Oracle MySQL compatible), `PYTHON`, `JAVA` | GraalVM: GraalJS, GraalPy, Java | all three languages, calls between them (polyglot), libraries shared across languages, the built-in vector and statistics library (`com.percona.mle.api`) |
| CPython worker | `CPYTHON` | the distribution's CPython (3.12+) with numpy | Python with little memory and a fast start |
| V8 worker | `JS` | the distribution's libnode (V8) | JavaScript with little memory and a fast start; the language name js_lang uses |

All three speak one protocol with the server, specified in `docs/protocol.md` (version 1.0), start through the
same launcher and sandbox, and give guest code the same SQL API.

A separate product: it builds against Percona Server and installs into it as a component (`component_mle`); the
server needs only its generic language router (`component_lang_router`, built with js_lang or
`-DWITH_LANG_ROUTER=ON`). Supported server lines: trunk-based releases (26.x), then 9.7.

*An MVP:* the protocol is specified and reviewed, the three workers implement it; not for production yet. Licensed
under the GNU General Public License version 2 (`LICENSE`), as Percona Server. Code comments cite the project's
design notes by name (e.g. `14-phase6-libraries.md`); those notes are not published.

## Layout

mysqld loads `component_mle`, which uses the host library to talk to workers; the workers themselves are started by
`mle_launcher`, a process of its own that sandboxes them (mysqld is not forked).

| Directory | What |
|---|---|
| `component/` | `component_mle`, the part inside mysqld: the language service (routines, libraries), sessions and their workers, SQL from routines, the UDFs (`mle_eval`, `mle_session_state`, ...), the `mle.*` system and status variables, accounts and time zones |
| `host/` | `mle_host`, the host library linked into the component (also the tests and the harness): the host side of the host–worker protocol (`worker.cc`), its codec, the client of the launcher and the launcher's own protocol, the default set-up of each worker class (`defaults.cc`) |
| `include/mle/` | the host library's headers: the protocol's constants (`protocol.h`), messages and values, workers, the launcher, the installed layout |
| `launcher/` | `mle_launcher`, the process that starts every worker: mailbox and socket per worker, warm pools, clone, Landlock, rlimits, the cgroup leaf with its limits (`cgroup.cc`), exits and OOM kills reported to mysqld |
| `worker/` | the workers. Its own files build the JVM worker's Java part (Maven, `pom.xml`): `mle-worker.jar`, the AOT cache, the GraalPy venv with numpy (`python/pom.xml`) |
| `worker/bootstrap/` | `mle_worker`, the JVM worker's executable: the seccomp filter (every worker kind installs it), then the JVM through JNI on a thread with a known stack |
| `worker/src/main/java/com/percona/mle/` | the JVM worker's Java code: `worker/` (protocol, dispatcher, GraalJS, GraalPy and Java routines, libraries, Oracle's JavaScript rules), `sql/` (the SQL API of routines), `api/` (the vector and statistics library) |
| `worker/cpython/` | `mle_cpython_worker`, the CPython worker: the embedding and the `_mlewire` extension in C++, the dispatcher and SQL API in Python (`python/mle_cpython/`) |
| `worker/v8/` | `mle_v8_worker`, the V8 worker on the distribution's libnode: C++ (values, modules, the worker) and the JavaScript it embeds (`api.js`, `sql.js`) |
| `worker/common/` | C++ shared by the native workers (CPython, V8): the mailbox, the wire reader and writer, the statements of §10.1, the RESULT writer |
| `tools/` | `mle_package` (the DBA's tool for npm, PyPI and Maven packages), `mle_aot_cache` (makes the AOT cache at install) |
| `harness/` | `mle_harness`, a command-line driver for workers without mysqld (development: start-up, calls, memory) |
| `unittest/` | GoogleTest tests (13 binaries: codec, launcher, protocol, conformance of all workers, failures, each worker kind), `mle_test_worker` (a scripted worker for protocol tests), `mle_proto_scripts` |
| `mysql-test/suite/component_mle/` | the MTR suite: `t/` and `r/`; `include/` (requirements, and the scripts that run JavaScript tests on V8 and Python tests on CPython); `std_data/` (a package directory) |
| `docs/` | `protocol.md`: the host–worker protocol, specification 1.0; `protocol-review.md`: its reviews, the differences from the code before it, the measurements; `limits.md`: known limits; `prewarming.md`: pools, pre-warming and worker reuse, for administrators |
| `perf/` | `regression.py` (two builds side by side), `bench.py` (every runtime of one build), `mle_perf.py` (what they share), their SQL; `e2e/`: against the same logic in an application over a network; `results/`: published runs (JSON) |
| `docker/` | the server + MLE image (`Dockerfile`), the DBA tools image (`Dockerfile.tools`, with a GraalPy launcher), compose, `run.sh` (start, SQL, status), `demo/` (the recorded demos' SQL) |
| `packaging/` | the packages' scripts: `debian/`, `rpm/`, the AppArmor profiles (`apparmor/`), the SELinux policy (`selinux/`) |
| `cmake/` | `packaging.cmake` (the packages: CPack, deb and rpm), `server.cmake` (building against a configured Percona Server tree), `mtr.sh.in` (the `mtr` wrapper of a build) |
| `ci.sh` | builds and runs every test, with cgroups, Landlock and all workers required |

## Build

Needs: a configured and built Percona Server (with the language router and its unit tests), a GraalVM Community
JDK 25 (`MLE_JAVA_HOME`, default `$JAVA_HOME`), Maven, network access to Maven Central and the GraalPy wheel index.
MLE uses GraalVM 25.6.5.1: the GraalJS, GraalPy and polyglot jars of that version (`graalvm.version` in
`worker/pom.xml` and `worker/python/pom.xml`), on the matching GraalVM Community JDK 25.0.5.1. The last commit
on GraalVM 25.5.5.0.3 is tagged `graalvm-25.5.5.0.3`.

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
the same launcher, sandbox, protocol and SQL API (exp design note 62). A worker takes 11–16 MB (PSS; 22–31 MB
with numpy) instead of hundreds, answers a new connection's first call in about 60 ms, and frees numpy memory at
once. The build installs numpy
(`MLE_CPYTHON_NUMPY`, default `numpy==2.4.4`) from PyPI into `cpython/site-packages`. In a build tree the
worker's Python code (`worker/cpython/python`) is linked, not copied: an edit needs no build, the next worker
reads it. Not on CPython: Java and JavaScript routines, cross-language libraries and the GraalVM-only helpers
(`com.percona.mle.api`: Stats, Vectors, Frame).

The build type follows the server build's (a debug server gives a debug MLE). For development the build links
`component_mle.so` and `mle/` into the server build's `plugin_output_directory`
(`-DMLE_LINK_INTO_SERVER_BUILD=OFF` to skip).

## Test

    ./ci.sh BUILD-mle                                    # build, every gtest and the MTR suite

`ci.sh` runs the tests in a delegated cgroup v2 (`systemd-run --user --scope -p Delegate=yes`), which the
workers' memory and process limits need, and turns the skips of a test that cannot run (no cgroup, no Landlock,
a worker kind not built) into failures. By hand:

    systemd-run --user --scope -q -p Delegate=yes BUILD-mle/mtr --parallel=4   # the MTR suite
    systemd-run --user --scope -q -p Delegate=yes BUILD-mle/runtime_output_directory/mle_codec-t   # each gtest

Without a delegated cgroup the memory-limit tests skip (`have_mle_cgroup.inc`). The benchmarks among the gtests
are disabled; `--gtest_also_run_disabled_tests` runs them.

Performance, a change against the commit before it (release builds, a quiet machine, the CPU governor
`performance`; about 25 minutes for 5 rounds):

    git worktree add ../percona-mle-base <commit>        # the code to compare with, built once:
    cmake -S ../percona-mle-base -B ../BUILD-mle-base -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMYSQL_SOURCE_DIR=... -DMYSQL_BUILD_DIR=<release server build> -DMLE_BUILD_JDK=ON \
      -DMLE_LINK_INTO_SERVER_BUILD=OFF -DMLE_WITH_UNIT_TESTS=OFF && ninja -C ../BUILD-mle-base
    perf/regression.py --server <release server build> --base ../BUILD-mle-base --new ../BUILD-mle-release

It runs each case of `perf/setup.sql` (the call path, values of every kind, SQL from routines read and written,
libraries, errors, vectors, a new connection's worker) on both builds in alternating order on the same data, and
exits with status 1 when a case is slower: the median over the rounds beyond the threshold (5 %), for the best
and for the median run of a round, and slower in all rounds but one.

## Performance

Absolute numbers of one build, every runtime (about 10 minutes), and an application against the same logic in a
stored procedure (about 8 minutes); `perf/README.md` and `perf/e2e/README.md` say what each case does:

    perf/bench.py --server <release server build> --mle ../BUILD-mle-release --wait-quiet --json bench.json
    perf/e2e/e2e.py --server <release server build> --mle ../BUILD-mle-release --wait-quiet --json e2e.json

**The run below:** 27 Nov 2026, full runs (not `--quick`), one run of each tool; the JSON with every measurement is
in `perf/results/2026-11-27/`. A developer laptop, not a dedicated machine: HP ZBook Power G10 A, AMD Ryzen 9
7940HS (8 cores, 16 threads), 64 GB, Ubuntu 26.04.1, kernel 7.0, CPU governor `performance`. Release builds
(RelWithDebInfo): MLE `1b6235a` (component and workers as on `main`), Percona Server 26.7.0 with js_lang and the
language router. GraalVM CE 25.6.5.1 (JDK 25.0.5.1), GraalPy 25.6.5 (Python 3.13), CPython 3.14.4, numpy 2.4.4, libnode
127. MLE defaults, except `mle.heap_max` 2 GB and `mle.memory_max` 4 GB in the end-to-end run. bench.py times
statements in the server and reports the best of 5 runs after a warm-up; lower is better everywhere except requests
per second.

Against GraalVM 25.5.5 (the code before the update, `perf/regression.py` on the same machine): no case slower;
Python routines with DECIMAL values 13–36 % faster.

Against the run of 26 Nov (`3b5a8e2`, `perf/results/2026-11-26/`): a small statement from a routine 33–46 % faster
(the host keeps a call's prepared statements); in CPython, numpy's 10 nearest 30 %, a `VECTOR` argument 33 % and a new
worker 25 % faster; GraalPy with numpy 13 % smaller and its new worker 19 % sooner. The applications of e2e.py were
3–12 % faster too, so part of the change is the machine's: compare columns of the same run.
GraalJS's sort of 200,000
objects (100 → 119 ms) is the spread of its runs: the median moved from 118 to 122 ms.

### Calls (bench.py)

µs per row over 1,000,000 rows, net of a plain scan of the same rows:

| | SQL (PSM) | js_lang | V8 worker | GraalJS | GraalPy | CPython | Java |
|---|---:|---:|---:|---:|---:|---:|---:|
| `a + b` | 1.04 | 1.16 | 1.31 | 1.26 | 1.26 | 1.28 | 1.21 |
| Levenshtein of two strings | 91.0 | 2.14 | 2.29 | 2.42 | 2.61 | 8.89 | 1.52 |

A call into a worker costs 0.1–0.3 µs more than an SQL stored function's, about what js_lang's V8 inside mysqld costs;
per-row logic runs 10–60 times faster than in an SQL stored function.

JavaScript, one call (ms):

| | js_lang | V8 worker | GraalJS |
|---|---:|---:|---:|
| `Math.sqrt(i) * Math.sin(i)`, 5,000,000 times | 216 | 219 | 123 |
| make and sort 200,000 objects | 49.9 | 51.7 | 119 |
| `JSON.stringify` 200,000 small objects, `JSON.parse` the array | 80.4 | 91.7 | 127 |

### SQL from routines (bench.py)

| | SQL (PSM) | V8 worker | GraalJS | GraalPy | CPython | Java |
|---|---:|---:|---:|---:|---:|---:|
| read 100,000 rows one by one (ms) | 91.7 | 26.7 | 24.2 | 26.6 | 35.0 | 19.7 |
| a small statement (µs each) | | 3.78 | 3.72 | 4.28 | 6.87 | 3.21 |
| `executeMany`, 1,000 rows (ms) | | 11.9 | 12.8 | 14.1 | 13.3 | 13.3 |

### Vectors (bench.py)

| | ms |
|---|---:|
| 10 nearest of 100,000 `VECTOR(768)`: SQL, exact | 99.7 |
| the same with an HNSW index (`M = 16`, default search settings; 5 of the exact 10 found) | 0.23 |
| the same in GraalPy, numpy | 272 |
| the same in CPython, numpy | 261 |
| the same in Java, the Vector API | 236 |
| related items of 10,000 vectors (cosine), the built-in library from Java / JavaScript / Python | 212 / 211 / 215 |
| the same, the library on one thread (`mle.vector_threads` is 4) | 802 |
| the same in numpy, one BLAS thread: GraalPy / CPython | 1,969 / 1,894 |

A `VECTOR(768)` argument costs 4.2 µs per row in GraalPy and 2.5 µs in CPython.

### A new connection's first call (bench.py)

ms at the client, median of 7 new `mysql` processes; a plain connection with no routine takes 5.2 ms:

| | new worker (pool off) | worker from the pool |
|---|---:|---:|
| js_lang (in mysqld) | 6.2 | |
| V8 worker | 30 | 8.8 |
| CPython | 58 | 15 |
| CPython, `import numpy` | 228 | 188 |
| GraalJS | 713 | 150 |
| Java | 626 | 53 |
| GraalPy | 2,104 | 1,708 |
| GraalPy, `import numpy` | 4,131 | 3,798 |

A worker from the pool has the polyglot engine ready and sets JavaScript and GraalPy up on their first use, which
keeps Java and Python workers smaller; `mle.pool_prewarm` sets them up in advance (`js`: GraalJS's first call in
about 20 ms; `python`; `numpy`).

### Memory per worker (bench.py)

PSS in MiB per worker, one worker alone and each of eight at once (they share code and the AOT cache):

| | one | each of eight |
|---|---:|---:|
| V8 worker | 38 | 10 |
| CPython | 16 | 11 |
| CPython, numpy | 31 | 22 |
| Java | 269 | 143 |
| GraalJS | 325 | 210 |
| GraalPy | 576 | 425 |
| GraalPy, numpy | 590 | 510 |

js_lang costs 0.5 MiB per session inside mysqld. The JVM worker maps its AOT cache at the cache's own address
(`-XX:ArchiveRelocationMode=0`); relocated, as the JDK does by default, each of eight GraalJS workers takes 277 MiB
instead of 210, and GraalPy with numpy 573 instead of 510.

### An application against the same logic in MLE (e2e.py)

Clients in a network namespace with 1 ms added delay (round trip 1.21 ms, an application server on a LAN); clients
and server on the same laptop. Median ms per request with one client (change against the application in the same
language), and requests per second with several clients:

| Case | Java app | `LANGUAGE JAVA` | Python app (numpy) | `LANGUAGE PYTHON` (GraalPy) | `LANGUAGE CPYTHON` |
|---|---:|---:|---:|---:|---:|
| `chatty`: 3 dependent queries, 200 HNSW candidates | 6.65 | 3.67 (−45 %) | 8.03 | 6.44 (−20 %) | 5.10 (−37 %) |
| `mmr500`: re-rank 500 candidates with their vectors | 4.64 | 3.93 (−15 %) | 6.96 | 7.48 (+7 %) | 6.93 (0 %) |
| `rescore1000`: rescore 1,000 candidates | 5.47 | 4.73 (−14 %) | 6.33 | 7.35 (+16 %) | 7.10 (+12 %) |
| `bulk`: ~5,000 vectors of a category | 25.9 | 16.5 (−36 %) | 39.4 | 20.3 (−48 %) | 20.4 (−48 %) |
| `wb200`: read 200 rows, score, upsert, commit | 7.19 | 4.42 (−38 %) | 6.88 | 6.28 (−9 %) | 6.25 (−9 %) |
| `wb1000`: the same with 1,000 rows | 13.9 | 10.2 (−27 %) | 17.6 | 14.0 (−21 %) | 14.7 (−16 %) |
| `wbvec200`: write back 200 rows with a `VECTOR(768)` each | 15.6 | 12.2 (−22 %) | 26.2 | 13.9 (−47 %) | 15.0 (−43 %) |
| `chatty`, 8 clients (req/s) | 1,223 | 1,660 | 905 | 884 | 1,295 |
| `mmr500`, 8 clients (req/s) | 1,188 | 1,486 | 784 | 878 | 955 |
| `wb200`, 4 clients (req/s) | 545 | 860 | 526 | 562 | 558 |

MLE wins where the application needs several round trips or moves many rows; where one query brings all the data
and the work is numpy's, the Python application is about as fast (`mmr500`) or 12–16 % faster (`rescore1000`).

### The protocol specification against the code before it

`perf/regression.py`, the specified protocol (1.0) against the code before the specification (`2a87613`):
calls 13–16 % faster, `TIMESTAMP` values 13–23 %, per-row logic, libraries and prepared and nested calls 4–10 %; the
other cases (rows, statements, writes, vectors, connections, cold start, idle memory) equal within measurement.
The component's subscription to query events adds about 0.1 µs to every statement on the server, MLE or not;
measured and accepted (`docs/limits.md`, "Every statement pays a little while the component is installed").
Since then the protocol carries a body of up to 40 bytes in the poster's own cache line: calls from Java and Python 1–3 % faster,
the others equal within measurement.

## First call and memory: pools, pre-warming, reuse

A session's first call is fast when its worker comes warm from the worker cache, or from a pool that prepared the
routine's language. JVM workers prepare Java and the polyglot engine; `mle.pool_prewarm` adds JavaScript (`js`),
GraalPy (`python`), numpy (`numpy`) and the vector library (`vectors`, the default), each for memory per worker:

| `mle.pool_prewarm` | Idle pool worker | First call from the pool: JavaScript / Python / Python with numpy / Java (ms) |
|---|---:|---|
| `vectors` (default) | 183 MB | 174 / 1,996 / 5,032 / 56 |
| `js,vectors` | 206 MB | 18 / 1,987 / 5,307 / 56 |
| `python,vectors` | 539 MB | 139 / 96 / 3,927 / 59 |
| `numpy,vectors` | 755 MB | 148 / 42 / 44 / 58 |

A GraalPy worker idle in its session gives 60–80 MB back after `mle.idle_trim_ms` (default 5000; 0: never).
`docs/prewarming.md` describes a worker's life, the pool and cache variables, the idle trim, how to choose, and what
to watch.

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
| `percona-mle-v8` | `MleV8` | the V8 worker (`-DMLE_V8=ON`): `LANGUAGE JS` on the distribution's libnode | common, libnode (`libnode127`, `nodejs-libs`) |
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
