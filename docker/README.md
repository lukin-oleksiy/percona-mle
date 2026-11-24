# Percona Server + MLE in Docker

Design and measured container constraints: `19-docker.md` (MLE design notes).

## Build

From a release build of Percona Server with the language router (`-DWITH_LANG_ROUTER=ON`, the default with js_lang)
and a release build of percona-mle against it (best with `-DMLE_BUILD_JDK=ON`), both fully built:

    docker/build-image.sh [--tools] ../BUILD-server-release ../BUILD-mle-release [percona-mle:dev]

The image (Ubuntu 26.04) holds the server and component_mle with its runtime in `<plugin_dir>/mle`, as the packages
install them (`21-packaging.md`): JavaScript, Java, Python + numpy, and the GraalVM runtime in `<plugin_dir>/mle/jdk`
(the trimmed runtime of `-DMLE_BUILD_JDK=ON`, else a copy of the JDK the build used). The JDK AOT cache for worker
start is made in the image (`mle_aot_cache`). `--tools` also builds the tools image, `<name>-tools:<tag>` of the
image's `<name>:<tag>` (`percona-mle-tools:dev`): mle_package with npm, Maven and a GraalPy of the workers' version.

## Run

    docker/run.sh start          # the server; prints the accounts and how to connect
    docker/run.sh sql            # the mysql client as the application account (also: run.sh sql < file.sql)
    docker/run.sh status | logs | stop | rm

`run.sh` needs only docker and the image, nothing else of this repository: it can be handed out with the image. It
keeps the data in a volume (`mle-data`), publishes the port on this host only (`MLE_BIND=0.0.0.0` for other hosts),
generates the passwords it is not given (`run.sh status` shows them) and takes its settings from the environment
(`MLE_IMAGE`, `MLE_NAME`, `MLE_PORT`, `MLE_MEMORY`, `MLE_PRIVILEGED=yes`; see its head). What it runs is:

    docker run -d --name mle -e MYSQL_ROOT_PASSWORD=secret -m 8g \
      -v mle-data:/var/lib/mysql percona-mle:dev

## Publish

    docker login registry.example.com
    docker/publish.sh push registry.example.com/team/percona-mle:0.1     # and ...-tools:0.1
    docker/publish.sh save /some/dir          # percona-mle-dev.tar.gz for `docker load -i`, with SHA-256 sums

Elsewhere: `MLE_IMAGE=registry.example.com/team/percona-mle:0.1 docker/run.sh start`.

First start: initializes the datadir, installs component_mle and component_lang_router, grants the MLE privileges
to root (and to `MLE_APP_USER`, optional, on its database). Passwords and names may hold any character. While
that runs, `/var/lib/mysql/.mle-first-start` exists; a data directory that still has the file is one whose first
start failed half way, and the container refuses to start on it: start again with an empty volume.

Arguments after the image name are mysqld's options (`docker run ... percona-mle:dev --mle.pool-size=4`); another
command runs instead of the server.

- **Default (unprivileged):** the MLE sandbox (Landlock, seccomp) works; workers have no cgroups of their own (the
  error log says so): the Java heap per worker and the container's memory limit apply, and on memory pressure the
  kernel kills workers first.
- **`--privileged`:** the entrypoint delegates the container's cgroup tree to mysqld; each worker gets its own
  cgroup (`mle.memory_max`, process limit), as on a host with systemd `Delegate=yes`.

Docker's default seccomp profile refuses `clone3`; the launcher then starts workers with `clone(CLONE_PIDFD)`.

Binary logging is on (server default). Without more settings, creating a stored function would need SUPER and a
`DETERMINISTIC`, `NO SQL` or `READS SQL DATA` function; the image sets `log_bin_trust_function_creators = ON`, which
lifts both checks, so that application accounts can create any function, as in a development setup. The server
warns at startup that this variable is deprecated. The clients in the container use UTF-8 (`LANG=C.UTF-8`).

## Packages (DBA)

    docker compose up -d
    docker compose run --rm tools install python networkx
    docker compose exec mysql mysql -uroot -pmle -e "SELECT mle_packages_refresh()"

The package volume is read-only in the server container.

`compose.yaml` is for development: its default passwords (root `mle`, app `app`) are well known, so it publishes the
port on 127.0.0.1 only. To reach the server from other hosts, set `MYSQL_ROOT_PASSWORD` and `MLE_APP_PASSWORD`
and then `MLE_BIND=0.0.0.0`.

## Requirements

Linux host with Landlock ABI ≥ 6 (kernel 6.12+; tested on 7.0), cgroup v2, Docker 24+ (tested 29.4, rootless).
