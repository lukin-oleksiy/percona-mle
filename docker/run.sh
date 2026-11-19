#!/bin/sh
# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Runs Percona Server with MLE from its Docker image: one container, its data
# in a named volume. Needs only docker and the image (built here with
# docker/build-image.sh, or pulled or loaded from where it was published);
# nothing else of this repository.
#
#   run.sh start            start the server (creates the container on first use)
#   run.sh sql [args]       the mysql client in the container, as the application
#                           account (run.sh sql -e "SELECT 1"; a script: run.sh sql < file.sql)
#   run.sh root [args]      the mysql client as root
#   run.sh status           the container, MLE's status, the passwords of this container
#   run.sh logs [-f]        the server's log (MLE's messages are there)
#   run.sh stop             stop the server; the data stays
#   run.sh rm               remove the container and its data volume
#
# Settings, from the environment (the defaults in brackets):
#   MLE_IMAGE      the image [percona-mle:dev]
#   MLE_NAME       the container's name; the volume is <name>-data [mle]
#   MLE_PORT       the port on this host [3306]
#   MLE_BIND       the address the port is published on [127.0.0.1: this host
#                  only; 0.0.0.0 lets other hosts connect]
#   MLE_MEMORY     the container's memory limit [8g]
#   MLE_PRIVILEGED yes: a privileged container, in which each MLE worker gets
#                  its own memory and process limits (mle.memory_max) [no]
#   MLE_WORKER     the worker build (mle.worker): JVM (Python, JavaScript and
#                  Java) or CPYTHON (Python only, on CPython) [AUTO: JVM]
#   MYSQL_ROOT_PASSWORD, MLE_APP_USER [app], MLE_APP_PASSWORD
#                  for the first start; passwords not given are generated and
#                  kept in the container's labels (run.sh status shows them)
set -eu

IMAGE=${MLE_IMAGE:-percona-mle:dev}
NAME=${MLE_NAME:-mle}
PORT=${MLE_PORT:-3306}
BIND=${MLE_BIND:-127.0.0.1}
MEMORY=${MLE_MEMORY:-8g}
APP_USER=${MLE_APP_USER:-app}

die() { echo "run.sh: $*" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || die "docker is not installed"

exists() { docker container inspect "$NAME" >/dev/null 2>&1; }
running() { [ "$(docker container inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" = true ]; }
label() { docker container inspect -f "{{index .Config.Labels \"mle.$1\"}}" "$NAME"; }
password() { LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20; }

wait_ready() {
  printf 'Waiting for the server'
  i=0
  while [ $i -lt 180 ]; do
    # The first start runs a setup server without networking first: ready is
    # the final server, which answers on TCP.
    if docker exec "$NAME" mysqladmin --protocol=tcp -h127.0.0.1 -uroot -p"$(label root-password)" ping >/dev/null 2>&1; then
      echo ' ready.'
      return 0
    fi
    running || { echo; docker logs --tail 20 "$NAME" >&2; die "the container stopped (its log is above)"; }
    printf '.'
    sleep 1
    i=$((i + 1))
  done
  echo
  die "the server did not answer in 3 minutes: see run.sh logs"
}

client() {  # user, password, the client's arguments
  user=$1 pass=$2
  shift 2
  running || die "the server is not running: run.sh start"
  tty=
  [ -t 0 ] && [ -t 1 ] && tty=-t
  docker exec -i $tty -e MYSQL_PWD="$pass" "$NAME" mysql -u"$user" --table "$@"
}

usage() { sed -n '/^# Runs Percona Server/,/^set -eu/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }

case "${1:-}" in
start)
  if exists; then
    running || docker start "$NAME" >/dev/null
  else
    docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull "$IMAGE" ||
      die "no image $IMAGE: build it (docker/build-image.sh) or set MLE_IMAGE"
    root_pw=${MYSQL_ROOT_PASSWORD:-$(password)}
    app_pw=${MLE_APP_PASSWORD:-$(password)}
    privileged=
    [ "${MLE_PRIVILEGED:-no}" = yes ] && privileged=--privileged
    worker=
    [ -n "${MLE_WORKER:-}" ] && worker=--loose-mle.worker=$MLE_WORKER
    docker run -d --name "$NAME" $privileged -m "$MEMORY" -p "$BIND:$PORT:3306" \
      -v "$NAME-data:/var/lib/mysql" \
      -e MYSQL_ROOT_PASSWORD="$root_pw" -e MLE_APP_USER="$APP_USER" -e MLE_APP_PASSWORD="$app_pw" \
      --label "mle.root-password=$root_pw" --label "mle.app-user=$APP_USER" --label "mle.app-password=$app_pw" \
      "$IMAGE" $worker >/dev/null
  fi
  wait_ready
  "$0" status
  ;;
sql)
  shift
  exists || die "no container $NAME: run.sh start"
  client "$(label app-user)" "$(label app-password)" "$(label app-user)" "$@"
  ;;
root)
  shift
  exists || die "no container $NAME: run.sh start"
  client root "$(label root-password)" "$@"
  ;;
status)
  exists || die "no container $NAME: run.sh start"
  docker ps -a --filter "name=^/$NAME\$" --format '{{.Names}}: {{.Status}}, image {{.Image}}, port {{.Ports}}'
  if running; then
    docker exec -e MYSQL_PWD="$(label root-password)" "$NAME" mysql -uroot -N -B -e "
      SELECT CONCAT('Percona Server ', VERSION(), ', MLE ', s.VARIABLE_VALUE, ', languages: ', l.VARIABLE_VALUE)
        FROM performance_schema.global_status s, performance_schema.global_status l
       WHERE s.VARIABLE_NAME = 'mle_status' AND l.VARIABLE_NAME = 'mle_languages_supported'" 2>/dev/null ||
      echo "(the server does not answer yet)"
  fi
  echo "Application account: $(label app-user) / $(label app-password)   (database $(label app-user))"
  echo "root: $(label root-password)"
  echo "Connect: $0 sql      or      mysql -h 127.0.0.1 -P $PORT -u $(label app-user) -p"
  ;;
logs)
  shift
  docker logs "$@" "$NAME"
  ;;
stop)
  running && docker stop "$NAME" >/dev/null
  echo "Stopped; the data is in the volume $NAME-data."
  ;;
rm)
  exists && docker rm -f "$NAME" >/dev/null
  docker volume rm "$NAME-data" >/dev/null 2>&1 || true
  echo "Removed the container $NAME and its data."
  ;;
-h|--help|help)
  usage
  ;;
*)
  usage >&2
  exit 1
  ;;
esac
