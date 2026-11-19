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

# Hands the built images to others: to a registry, or as a file.
#
#   publish.sh push REPOSITORY[:TAG]   tags the server image as REPOSITORY:TAG and the tools
#                                      image as REPOSITORY-tools:TAG and pushes both (log in
#                                      to the registry first: docker login REGISTRY)
#   publish.sh save [DIRECTORY]        writes percona-mle-<tag>.tar.gz (and the tools image's)
#                                      for `docker load -i`, with their SHA-256 sums
#
# The images are MLE_IMAGE [percona-mle:dev] and <its name>-tools:<its tag>; the tools image
# (mle_package with npm, Maven, GraalPy) is left out when it does not exist. TAG defaults to
# the image's tag. Whoever gets the images runs them with docker/run.sh (MLE_IMAGE=...).
set -eu

IMAGE=${MLE_IMAGE:-percona-mle:dev}
die() { echo "publish.sh: $*" >&2; exit 1; }
usage() { sed -n '/^# Hands the built images/,/^set -eu/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }

# name:tag of the image, split on the last colon of the last path component.
case "${IMAGE##*/}" in
  *:*) NAME=${IMAGE%:*} TAG=${IMAGE##*:} ;;
  *) NAME=$IMAGE TAG=latest ;;
esac
TOOLS="$NAME-tools:$TAG"
docker image inspect "$IMAGE" >/dev/null 2>&1 || die "no image $IMAGE: build it with docker/build-image.sh"
have_tools=no
docker image inspect "$TOOLS" >/dev/null 2>&1 && have_tools=yes

case "${1:-}" in
push)
  target=${2:-}
  [ -n "$target" ] || die "push where? publish.sh push REPOSITORY[:TAG]"
  case "${target##*/}" in
    *:*) repo=${target%:*} tag=${target##*:} ;;
    *) repo=$target tag=$TAG ;;
  esac
  docker tag "$IMAGE" "$repo:$tag"
  docker push "$repo:$tag"
  if [ $have_tools = yes ]; then
    docker tag "$TOOLS" "$repo-tools:$tag"
    docker push "$repo-tools:$tag"
  fi
  echo "Pushed. To run it elsewhere: MLE_IMAGE=$repo:$tag docker/run.sh start"
  ;;
save)
  dir=${2:-.}
  [ -d "$dir" ] || die "no directory $dir"
  base=$(basename "$NAME")
  out="$dir/$base-$TAG.tar.gz"
  echo "Writing $out ..."
  docker save "$IMAGE" | gzip > "$out"
  files=$out
  if [ $have_tools = yes ]; then
    echo "Writing $dir/$base-tools-$TAG.tar.gz ..."
    docker save "$TOOLS" | gzip > "$dir/$base-tools-$TAG.tar.gz"
    files="$files $dir/$base-tools-$TAG.tar.gz"
  fi
  # shellcheck disable=SC2086
  (cd "$dir" && sha256sum $(for f in $files; do basename "$f"; done) > "$base-$TAG.sha256")
  ls -lh $files "$dir/$base-$TAG.sha256"
  echo "To use: docker load -i $(basename "$out"), then MLE_IMAGE=$IMAGE docker/run.sh start"
  ;;
-h|--help|help)
  usage
  ;;
*)
  usage >&2
  exit 1
  ;;
esac
