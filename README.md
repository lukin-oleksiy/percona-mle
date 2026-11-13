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
| `worker/` | `mle_worker` (a JNI launcher of the JVM) and `mle-worker.jar` (GraalJS, GraalPy, Java routines, the SQL API); Maven; the numpy venv; the AOT cache; `native/`: the Python-only native build |
| `tools/` | `mle_package` (the DBA's npm/PyPI/Maven tool), `mle_aot_cache` (makes the AOT cache at install) |
| `harness/` | a command-line driver for workers (development) |
| `unittest/` | GoogleTest unit tests (8 binaries) |
| `mysql-test/suite/component_mle` | the MTR suite |
| `docker/` | the server + MLE image, the DBA tools image, compose |
| `packaging/`, `cmake/packaging.cmake` | packages (CPack, deb and rpm) and their scripts |
| `cmake/server.cmake` | building against a configured Percona Server tree |

## Build

Needs: a configured and built Percona Server (with the language router and its unit tests), a GraalVM Community
JDK 25 (`MLE_JAVA_HOME`, default `$JAVA_HOME`), Maven, network access to Maven Central and the GraalPy wheel index.

    cmake -G Ninja -S percona-mle -B BUILD-mle \
          -DMYSQL_SOURCE_DIR=<server source> -DMYSQL_BUILD_DIR=<server build> [-DMLE_BUILD_JDK=ON]
    ninja -C BUILD-mle

**The Python-only native build** (`-DMLE_PYTHON_NATIVE=ON`, optional): the worker as a GraalVM Native Image
library that `mle_worker` loads instead of the JVM — highly optimised for Python-only use: about half the memory
per worker, a worker starts in about 0.2 s instead of about 2.5 s, no JDK. It runs Python routines only (no Java,
no JavaScript). Building it takes a few minutes and about 12 GB of RAM. `mle.worker` (read-only) selects the build:
`AUTO` (the JVM build when it is installed, else the native one), `JVM` or `NATIVE`.

The build type follows the server build's (a debug server gives a debug MLE). For development the build links
`component_mle.so` and `mle/` into the server build's `plugin_output_directory`
(`-DMLE_LINK_INTO_SERVER_BUILD=OFF` to skip).

## Test

    BUILD-mle/mtr --parallel=4                           # the MTR suite on the server build
    systemd-run --user --scope -q -p Delegate=yes BUILD-mle/runtime_output_directory/mle_codec-t   # each gtest

## Install and packages

Packages (`cpack -G DEB` or `cpack -G RPM` in the build directory; the server must be built with
`INSTALL_LAYOUT=DEB` or `RPM`), one per install component (`cmake --install BUILD-mle --component <c>`):

| Package | Component | Contents | Needs |
|---|---|---|---|
| `percona-mle-common` | `MleCommon` | `component_mle`, `mle_launcher`, `mle_worker` | the server |
| `percona-mle` | `MleCore` | the JVM build: worker jar, runtime jars, JavaScript and Java | common, jdk |
| `percona-mle-jdk` | `MleJdk` | a trimmed GraalVM runtime (`-DMLE_BUILD_JDK=ON`) | |
| `percona-mle-python-runtime` | `MlePythonRuntime` | Python's standard library and the numpy venv (both builds) | common |
| `percona-mle-python` | `MlePython` | GraalPy for the JVM build | percona-mle, python-runtime |
| `percona-mle-python-native` | `MlePythonNative` | the Python-only native build (`-DMLE_PYTHON_NATIVE=ON`) | common, python-runtime |
| `percona-mle-tools` | `MleTools` | `mle_package` (third-party npm, PyPI, Maven packages) | percona-mle |
| `percona-mle-selinux` | `MleSelinux` | the SELinux policy module (RPM only; `-DMLE_SELINUX=ON`, the default with selinux-policy-devel) | pulled by common where SELinux policy is installed |

With the JVM build, `<plugin dir>/mle/mle_aot_cache` makes the JDK AOT cache that workers start from (the
`percona-mle` package runs it: Debian triggers, RPM file triggers, once per transaction that changes the JDK,
jars or Python).

SELinux (`packaging/selinux/percona_mle.te`): mysqld (`mysqld_t`) starts `mle_launcher` in `mle_launcher_t`
(cgroups, mailboxes; no JIT), which starts workers in `mle_worker_t` (`execmem` for the JIT compilers); mysqld
gains only the workers' descriptors and signals. On RHEL-like kernels Landlock must be enabled on the kernel
command line (`lsm=landlock,...`). In the server:

    INSTALL COMPONENT 'file://component_lang_router';
    INSTALL COMPONENT 'file://component_mle';
    GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO ...;

Linux only: Landlock ABI ≥ 6 (kernel 6.12+), cgroup v2 with a delegated cgroup for per-worker memory limits.
x86-64 so far.
