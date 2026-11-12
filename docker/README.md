# Percona Server + MLE in Docker (POC)

Design and measured container constraints: `19-docker.md` (MLE design notes).

## Build

From a release build tree configured with `-DWITH_MLE=ON` (best with `-DMLE_BUILD_JDK=ON`) and fully built:

    components/mle/docker/build-image.sh [--tools] ../BUILD-release [percona-mle:dev]

The image (Ubuntu 26.04) holds the server and component_mle with its runtime in `<plugin_dir>/mle`, as the packages
install them (`21-packaging.md`): JavaScript, Java, Python + numpy, and the GraalVM runtime in `<plugin_dir>/mle/jdk`
(the trimmed runtime of `-DMLE_BUILD_JDK=ON`, else a copy of the JDK the build used). The JDK AOT cache for worker
start is made in the image (`mle_aot_cache`). `--tools` also builds `percona-mle-tools:dev` (mle_package with npm,
Maven and a GraalPy of the workers' version).

## Run

    docker run -d --name mle -e MYSQL_ROOT_PASSWORD=secret -m 8g \
      -v mle-data:/var/lib/mysql percona-mle:dev

First start: initializes the datadir, installs component_mle and component_lang_router, grants the MLE privileges
to root (and to `MLE_APP_USER`, optional, on its database).

- **Default (unprivileged):** the MLE sandbox (Landlock, seccomp) works; workers have no cgroups of their own (the
  error log says so): the Java heap per worker and the container's memory limit apply, and on memory pressure the
  kernel kills workers first.
- **`--privileged`:** the entrypoint delegates the container's cgroup tree to mysqld; each worker gets its own
  cgroup (`mle.memory_max`, process limit), as on a host with systemd `Delegate=yes`.

Docker's default seccomp profile refuses `clone3`; the launcher then starts workers with `clone(CLONE_PIDFD)`.

Binary logging is on (server default): functions need `DETERMINISTIC`, `NO SQL` or `READS SQL DATA` (or
`log_bin_trust_function_creators`).

## Packages (DBA)

    docker compose up -d
    docker compose run --rm tools install python networkx
    docker compose exec mysql mysql -uroot -pmle -e "SELECT mle_packages_refresh()"

The package volume is read-only in the server container.

## Requirements

Linux host with Landlock ABI ≥ 6 (kernel 6.12+; tested on 7.0), cgroup v2, Docker 24+ (tested 29.4, rootless).
