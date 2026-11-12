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

*Proof of concept, confidential.*

## Layout

| Directory | What |
|---|---|
| `component/` | `component_mle`: the server side (language service, sessions, SQL for routines, UDFs, variables) |
| `host/`, `include/mle/` | the host library: wire protocol and codec, launcher and worker clients, the installed layout |
| `launcher/` | `mle_launcher`: starts and sandboxes workers (clone, Landlock, seccomp, cgroups) |
| `worker/` | `mle_worker` (a JNI launcher of the JVM) and `mle-worker.jar` (GraalJS, GraalPy, Java routines, the SQL API); Maven; the numpy venv; the AOT cache |
| `tools/` | `mle_package` (the DBA's npm/PyPI/Maven tool), `mle_aot_cache` (makes the AOT cache at install) |
| `harness/` | a command-line driver for workers (development) |
| `unittest/` | GoogleTest unit tests (8 binaries) |
| `mysql-test/suite/component_mle` | the MTR suite |
| `docker/` | the server + MLE image, the DBA tools image, compose |
| `packaging/` | package scripts (deb); the full deb/rpm packaging is still to be ported (22-separate-product.md §4) |
| `cmake/server.cmake` | building against a configured Percona Server tree |

## Build

Needs: a configured and built Percona Server (with the language router and its unit tests), a GraalVM Community
JDK 25 (`MLE_JAVA_HOME`, default `$JAVA_HOME`), Maven, network access to Maven Central and the GraalPy wheel index.

    cmake -G Ninja -S percona-mle -B BUILD-mle \
          -DMYSQL_SOURCE_DIR=<server source> -DMYSQL_BUILD_DIR=<server build> [-DMLE_BUILD_JDK=ON]
    ninja -C BUILD-mle

The build type follows the server build's (a debug server gives a debug MLE). For development the build links
`component_mle.so` and `mle/` into the server build's `plugin_output_directory`
(`-DMLE_LINK_INTO_SERVER_BUILD=OFF` to skip).

## Test

    BUILD-mle/mtr --parallel=4                           # the MTR suite on the server build
    systemd-run --user --scope -q -p Delegate=yes BUILD-mle/runtime_output_directory/mle_codec-t   # each gtest

## Install and packages

`cmake --install BUILD-mle --component <c>` with the components of the packages: `MleCore` (`percona-mle`),
`MlePython` (`percona-mle-python`), `MleJdk` (`percona-mle-jdk`, with `-DMLE_BUILD_JDK=ON`), `MleTools`
(`percona-mle-tools`). After installing, `<plugin dir>/mle/mle_aot_cache` makes the JDK AOT cache that workers
start from (the packages run it). In the server:

    INSTALL COMPONENT 'file://component_lang_router';
    INSTALL COMPONENT 'file://component_mle';
    GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO ...;

Linux only: Landlock ABI ≥ 6 (kernel 6.12+), cgroup v2 with a delegated cgroup for per-worker memory limits.
