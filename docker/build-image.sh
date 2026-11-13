#!/bin/bash
# Builds the percona-mle image (19-docker.md) from a release build of Percona
# Server (with the language router) and a release build of percona-mle
# against it: installs the server and the MLE install components (the
# packages' split, 21-packaging.md) into a staging directory, with the JDK as
# <mle dir>/jdk: the trimmed runtime of an MLE build with -DMLE_BUILD_JDK=ON,
# else a copy of the GraalVM JDK the build used (MLE_JAVA_HOME). Then runs
# docker build.
#
#   docker/build-image.sh [--tools] <server build> <mle build> [image tag]
#
# --tools also builds <image>-tools (mle_package with npm, Maven, GraalPy).
set -euo pipefail
TOOLS=no
if [ "${1:-}" = "--tools" ]; then TOOLS=yes; shift; fi
USAGE="usage: build-image.sh [--tools] <server build> <mle build> [image tag]"
SERVER=$(realpath "${1:?$USAGE}")
BUILD=$(realpath "${2:?$USAGE}")
TAG=${3:-percona-mle:dev}
HERE=$(dirname "$(realpath "$0")")
CTX=$(mktemp -d "${TMPDIR:-/tmp}/percona-mle.XXXXXX")
trap 'rm -rf "$CTX"' EXIT

grep -q '^CMAKE_PROJECT_NAME:STATIC=percona-mle$' "$BUILD/CMakeCache.txt" ||
  { echo "build-image.sh: $BUILD is not a build of percona-mle" >&2; exit 1; }
grep -q '^WITH_LANG_ROUTER:BOOL=ON' "$SERVER/CMakeCache.txt" ||
  { echo "build-image.sh: $SERVER is not a server build with the language router" >&2; exit 1; }
JAVA_HOME_BUILD=$(sed -n 's/^MLE_JAVA_HOME:PATH=//p' "$BUILD/CMakeCache.txt")
MLE_DIR="$CTX/stage/lib/plugin/mle"

echo "Installing $SERVER and $BUILD into the image context"
for c in Server Client SharedLibraries; do
  cmake --install "$SERVER" --prefix "$CTX/stage" --component "$c" >> "$CTX/install.log"
done
for c in MleCommon MleCore MlePythonRuntime MlePython MleTools; do
  cmake --install "$BUILD" --prefix "$CTX/stage" --component "$c" >> "$CTX/install.log"
done
if grep -q '^MLE_BUILD_JDK:BOOL=ON' "$BUILD/CMakeCache.txt"; then
  cmake --install "$BUILD" --prefix "$CTX/stage" --component MleJdk >> "$CTX/install.log"
else
  echo "Adding the GraalVM JDK from $JAVA_HOME_BUILD (no -DMLE_BUILD_JDK=ON)"
  cp -a "$(realpath "$JAVA_HOME_BUILD")/." "$MLE_DIR/jdk/"
  rm -rf "$MLE_DIR/jdk/lib/src.zip" "$MLE_DIR/jdk/jmods"
fi
# Not needed in a server image.
rm -rf "$CTX/stage/mysql-test" "$CTX/stage/man" "$CTX/stage/docs" \
       "$CTX/stage/include" "$CTX/stage/lib/mysqlrouter" 2>/dev/null || true


cp "$HERE/Dockerfile" "$HERE/my.cnf" "$HERE/mle-entrypoint.sh" "$CTX/"
du -sh "$CTX/stage" "$MLE_DIR/jdk"
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
