#!/bin/bash
# Builds the percona-mle image from a release build tree configured with
# -DWITH_MLE=ON (19-docker.md): installs it into a staging directory, adds
# the GraalVM JDK the build used (MLE_JAVA_HOME), and runs docker build.
#
#   components/mle/docker/build-image.sh [--tools] <build dir> [image tag]
#
# --tools also builds <image>-tools (mle_package with npm, Maven, GraalPy).
set -euo pipefail
TOOLS=no
if [ "${1:-}" = "--tools" ]; then TOOLS=yes; shift; fi
BUILD=$(realpath "${1:?usage: build-image.sh [--tools] <build dir> [image tag]}")
TAG=${2:-percona-mle:dev}
HERE=$(dirname "$(realpath "$0")")
CTX=$(mktemp -d "${TMPDIR:-/tmp}/percona-mle.XXXXXX")
trap 'rm -rf "$CTX"' EXIT

grep -q '^WITH_MLE:BOOL=ON' "$BUILD/CMakeCache.txt" ||
  { echo "build-image.sh: $BUILD is not configured with -DWITH_MLE=ON" >&2; exit 1; }
JAVA_HOME_BUILD=$(sed -n 's/^MLE_JAVA_HOME:PATH=//p' "$BUILD/CMakeCache.txt")

echo "Installing $BUILD into the image context"
for c in Server Client SharedLibraries MleWorker; do
  cmake --install "$BUILD" --prefix "$CTX/stage" --component "$c" >> "$CTX/install.log"
done
# Not needed in a server image.
rm -rf "$CTX/stage/mysql-test" "$CTX/stage/man" "$CTX/stage/docs" \
       "$CTX/stage/include" "$CTX/stage/lib/mysqlrouter" 2>/dev/null || true

echo "Adding the GraalVM JDK from $JAVA_HOME_BUILD"
cp -a "$(realpath "$JAVA_HOME_BUILD")/." "$CTX/graalvm/"
rm -rf "$CTX/graalvm/lib/src.zip" "$CTX/graalvm/jmods"

cp "$HERE/Dockerfile" "$HERE/my.cnf" "$HERE/mle-entrypoint.sh" "$CTX/"
du -sh "$CTX/stage" "$CTX/graalvm"
docker build -t "$TAG" "$CTX"

if [ "$TOOLS" = yes ]; then
  VERSION=$(sed -n 's:.*<graalvm.version>\(.*\)</graalvm.version>.*:\1:p' "$HERE/../worker/pom.xml")
  TOOLS_TAG="${TAG%%:*}-tools:${TAG#*:}"
  TOOLS_CTX=$(mktemp -d "${TMPDIR:-/tmp}/percona-mle-tools.XXXXXX")
  cp "$HERE/Dockerfile.tools" "$HERE/graalpy" "$TOOLS_CTX/"
  docker build -t "$TOOLS_TAG" -f "$TOOLS_CTX/Dockerfile.tools" \
    --build-arg BASE="$TAG" --build-arg GRAALVM_VERSION="$VERSION" "$TOOLS_CTX"
  rm -rf "$TOOLS_CTX"
fi
