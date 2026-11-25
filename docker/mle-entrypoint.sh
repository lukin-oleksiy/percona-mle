#!/bin/sh
# Entrypoint of the percona-mle image (19-docker.md):
#  - first start: initialize the datadir, install component_mle (and the
#    language router), set the root password, create the application account;
#  - every start: if the cgroup filesystem is writable (--privileged), give
#    mysqld a delegated cgroup so that MLE workers get their own memory and
#    process limits (mle.memory_max); otherwise the container's own limits
#    apply and MLE says so in the error log;
#  - exec mysqld.
# Environment: MYSQL_ROOT_PASSWORD (or MYSQL_ALLOW_EMPTY_PASSWORD=yes),
# MLE_APP_USER / MLE_APP_PASSWORD (optional: an account with the MLE
# privileges on its own database MLE_APP_DATABASE, default the user name).
# Arguments: mysqld's options (with or without "mysqld" before them), or
# another command to run instead.
set -eu

# Only options: they are mysqld's.
case "${1:-}" in -*) set -- mysqld "$@" ;; esac
if [ "${1:-mysqld}" != "mysqld" ]; then exec "$@"; fi
[ $# -eq 0 ] || shift  # (dash ends the script on a shift of nothing)
DATADIR=/var/lib/mysql
# There while the first start does its setup, gone when all of it is done:
# the data directory alone does not tell (mysqld --initialize makes it before
# the accounts exist). In /var/lib/mysql's parent would not survive with the
# volume, so it is a file of the data directory.
MARKER="$DATADIR/.mle-first-start"

# The values below come from the environment: any character may be in them.
# They are written with printf (dash's echo takes backslash sequences), never
# through a command substitution (it drops trailing newlines).
# sql_string VALUE: the inside of a '...' string literal, for a session
# without NO_BACKSLASH_ESCAPES (first_start sets its sql_mode): \ becomes
# \\ and ' becomes ''. Nothing else is special there, to the server or to
# the mysql client.
sql_string() {
  printf '%s' "$1" | LC_ALL=C sed -e 's/\\/\\\\/g' -e "s/'/''/g"
}
# sql_name VALUE: the inside of a `...` identifier: ` becomes ``.
sql_name() {
  printf '%s' "$1" | LC_ALL=C sed -e 's/`/``/g'
}
# sql_name_pattern VALUE: the same for a database name in GRANT ... ON
# `name`.*, which is a LIKE pattern: \, _ and % stand for themselves with a
# \ before them.
sql_name_pattern() {
  printf '%s' "$1" | LC_ALL=C sed -e 's/[\\_%]/\\&/g' -e 's/`/``/g'
}

delegate_cgroup() {
  cg=/sys/fs/cgroup
  [ -w "$cg/cgroup.procs" ] || return 0
  mkdir -p "$cg/init" "$cg/mysqld" 2>/dev/null || return 0
  # cgroup v2: a cgroup with controllers for its children has no processes of
  # its own; move tini and anything else out of the root first.
  for p in $(cat "$cg/cgroup.procs"); do
    [ "$p" = "$$" ] && continue
    echo "$p" > "$cg/init/cgroup.procs" 2>/dev/null || true
  done
  echo "$$" > "$cg/mysqld/cgroup.procs"
  echo "+memory +pids" > "$cg/cgroup.subtree_control" 2>/dev/null || return 0
  # Delegate the subtree to mysqld's OS user (as systemd's Delegate=yes):
  # mle.cgroup_base=AUTO then uses the parent of mysqld's cgroup.
  chown mysql:mysql "$cg" "$cg/cgroup.procs" "$cg/cgroup.subtree_control" \
    "$cg/mysqld" "$cg/mysqld/cgroup.procs" 2>/dev/null || true
  echo "mle-entrypoint: worker cgroups delegated below $cg"
}

first_start() {
  echo "mle-entrypoint: initializing $DATADIR"
  mysqld --initialize-insecure --user=mysql --datadir="$DATADIR"
  touch "$MARKER"  # (after: mysqld wants an empty directory to initialize)
  mysqld --user=mysql --skip-networking --socket=/var/run/mysqld/init.sock &
  pid=$!
  up=no
  for i in $(seq 120); do
    if mysqladmin --socket=/var/run/mysqld/init.sock -uroot ping >/dev/null 2>&1; then
      up=yes
      break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  if [ "$up" != yes ]; then
    echo "mle-entrypoint: the server did not start for the setup" >&2
    kill "$pid" 2>/dev/null || true
    wait "$pid" || true
    exit 1
  fi
  # component_mle registers all five, whichever workers are installed.
  mle_privileges="CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE, CREATE_JS_ROUTINE, CREATE_CPYTHON_ROUTINE"
  # --binary-mode: the client passes the bytes on as they are (no \r\n to
  # \n). The first SQL error ends the batch and, with set -e, this script.
  {
    # This container's own setup: not in its binary log, which a replica of it would replay
    # (CREATE USER root@'%' fails there; MVP review OPS-3).
    printf '%s\n' "SET @@SESSION.sql_log_bin = 0;"
    printf '%s\n' "SET @@SESSION.sql_mode = '';"
    printf '%s\n' "INSTALL COMPONENT 'file://component_mle';"
    printf '%s\n' "INSTALL COMPONENT 'file://component_lang_router';"
    if [ -n "${MYSQL_ROOT_PASSWORD:-}" ]; then
      printf '%s' "ALTER USER 'root'@'localhost' IDENTIFIED BY '"
      sql_string "$MYSQL_ROOT_PASSWORD"
      printf '%s\n' "';"
      printf '%s' "CREATE USER 'root'@'%' IDENTIFIED BY '"
      sql_string "$MYSQL_ROOT_PASSWORD"
      printf '%s\n' "';"
      printf '%s\n' "GRANT ALL ON *.* TO 'root'@'%' WITH GRANT OPTION;"
    fi
    printf '%s\n' "GRANT $mle_privileges ON *.* TO 'root'@'localhost';"
    if [ -n "${MLE_APP_USER:-}" ]; then
      db=${MLE_APP_DATABASE:-$MLE_APP_USER}
      printf '%s' 'CREATE DATABASE IF NOT EXISTS `'
      sql_name "$db"
      printf '%s\n' '`;'
      printf '%s' "CREATE USER '"
      sql_string "$MLE_APP_USER"
      printf '%s' "'@'%' IDENTIFIED BY '"
      sql_string "${MLE_APP_PASSWORD:-}"
      printf '%s\n' "';"
      printf '%s' 'GRANT ALL ON `'
      sql_name_pattern "$db"
      printf '%s' "\`.* TO '"
      sql_string "$MLE_APP_USER"
      printf '%s\n' "'@'%';"
      printf '%s' "GRANT $mle_privileges ON *.* TO '"
      sql_string "$MLE_APP_USER"
      printf '%s\n' "'@'%';"
    fi
    # In this session: root may have a password from here on.
    printf '%s\n' "SHUTDOWN;"
  } | mysql --binary-mode --socket=/var/run/mysqld/init.sock -uroot
  wait "$pid"
  rm -f "$MARKER"
}

if [ -e "$MARKER" ]; then
  # Starting this server would run it without the component, the accounts
  # and the root password.
  echo "mle-entrypoint: $MARKER is there: the first start of this container" \
    "did not finish its setup (see its log). Start again with an empty volume." >&2
  exit 1
fi
if [ ! -d "$DATADIR/mysql" ]; then
  if [ -z "${MYSQL_ROOT_PASSWORD:-}" ] && [ "${MYSQL_ALLOW_EMPTY_PASSWORD:-}" != "yes" ]; then
    echo "mle-entrypoint: set MYSQL_ROOT_PASSWORD (or MYSQL_ALLOW_EMPTY_PASSWORD=yes)" >&2
    exit 1
  fi
  delegate_cgroup  # before the first start: its server runs workers too
  first_start
else
  delegate_cgroup
fi

exec mysqld "$@"
