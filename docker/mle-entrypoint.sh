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
set -eu

if [ "${1:-mysqld}" != "mysqld" ]; then exec "$@"; fi
shift || true
DATADIR=/var/lib/mysql

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
  mysqld --user=mysql --skip-networking --socket=/var/run/mysqld/init.sock &
  pid=$!
  for i in $(seq 120); do
    mysqladmin --socket=/var/run/mysqld/init.sock -uroot ping >/dev/null 2>&1 && break
    sleep 1
  done
  {
    echo "INSTALL COMPONENT 'file://component_mle';"
    echo "INSTALL COMPONENT 'file://component_lang_router';"
    if [ -n "${MYSQL_ROOT_PASSWORD:-}" ]; then
      echo "ALTER USER 'root'@'localhost' IDENTIFIED BY '${MYSQL_ROOT_PASSWORD}';"
      echo "CREATE USER 'root'@'%' IDENTIFIED BY '${MYSQL_ROOT_PASSWORD}';"
      echo "GRANT ALL ON *.* TO 'root'@'%' WITH GRANT OPTION;"
    fi
    echo "GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO 'root'@'localhost';"
    if [ -n "${MLE_APP_USER:-}" ]; then
      db=${MLE_APP_DATABASE:-$MLE_APP_USER}
      echo "CREATE DATABASE IF NOT EXISTS \`$db\`;"
      echo "CREATE USER '${MLE_APP_USER}'@'%' IDENTIFIED BY '${MLE_APP_PASSWORD:-}';"
      echo "GRANT ALL ON \`$db\`.* TO '${MLE_APP_USER}'@'%';"
      echo "GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO '${MLE_APP_USER}'@'%';"
    fi
    # In this session: root may have a password from here on.
    echo "SHUTDOWN;"
  } | mysql --socket=/var/run/mysqld/init.sock -uroot
  wait "$pid" || true
}

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
