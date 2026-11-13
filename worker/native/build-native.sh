#!/bin/bash
# The Python-only native build of the worker (exp design note 23): a Native Image shared library
# that the worker bootstrap (mle_worker) loads instead of libjvm.so. Same sources as the JVM
# worker: mle-worker.jar with its dependencies, without GraalJS (and without Java routines,
# which a Native Image cannot compile and load). Needs ≈ 13-16 GB RAM and a few minutes.
#
#   build-native.sh <java_home> <mle-worker.jar> <deps dir> <output dir>
#
# Output: <output dir>/libmleworker.so (+ the image's headers) and the language resources
# Native Image copies beside it.
set -euo pipefail
JAVA_HOME=$1 JAR=$(realpath "$2") DEPS=$(realpath "$3") OUT=$(realpath -m "$4")
# The jar's manifest Class-Path names every dependency (GraalJS too), and Native Image follows
# it: the worker's classes and resources go on the class path as a directory instead.
CLASSES=$OUT-classes  # beside the output: workers read the output directory
rm -rf "$CLASSES"
mkdir -p "$CLASSES"
(cd "$CLASSES" && unzip -q "$JAR" -x META-INF/MANIFEST.MF)
CP=$CLASSES
for j in "$DEPS"/*.jar; do
  case "$(basename "$j")" in
    js-language-*|profiler-tool-*) continue ;;  # no GraalJS, no profiler in this build
  esac
  CP=$CP:$j
done
mkdir -p "$OUT"
exec "$JAVA_HOME/bin/native-image" \
  --shared -o "$OUT/libmleworker" -cp "$CP" \
  --no-fallback \
  -J-Xmx${MLE_NATIVE_BUILD_HEAP:-14g} \
  --enable-native-access=ALL-UNNAMED \
  -H:+UnlockExperimentalVMOptions -H:+JNIExportSymbols -H:+ForeignAPISupport -H:-UnlockExperimentalVMOptions \
  -Dpolyglot.engine.WarnInterpreterOnly=false \
  ${MLE_NATIVE_IMAGE_OPTIONS:-}
