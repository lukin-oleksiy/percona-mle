#!/bin/bash
# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
# Licensed under the GNU General Public License, version 2 (see LICENSE).
#
# ci.sh: what a change of percona-mle must pass (MVP review D9 (3), TEST-7). Builds the tree, runs
# every unit-test binary and the MySQL tests of component_mle, and says what failed.
#
#   ./ci.sh BUILD_DIR [--parallel N] [--skip-build] [--skip-unit] [--skip-mtr] [MTR_TEST ...]
#
# BUILD_DIR is a configured percona-mle build (cmake ... -DMLE_SERVER_BUILD=...; README "Build"):
# it has runtime_output_directory/mle_*-t and the mtr wrapper. The tests run in a delegated cgroup
# v2 (systemd-run --user --scope -p Delegate=yes), as the workers' memory and process limits need
# one, and with skips turned into failures: no cgroup, no Landlock, or a worker kind that is not
# built (CPython, V8) fails the run instead of passing it quietly. MTR_TEST names run only those.
#
# Exit status: 0 when everything passed, 1 when something failed, 2 for bad arguments.

set -u
usage() { sed -n '/^# ci.sh/,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

build="" parallel=4 do_build=1 do_unit=1 do_mtr=1 tests=()
while [ $# -gt 0 ]; do
  case "$1" in
    --parallel) parallel="${2:?}"; shift ;;
    --skip-build) do_build=0 ;;
    --skip-unit) do_unit=0 ;;
    --skip-mtr) do_mtr=0 ;;
    -h|--help) usage ;;
    -*) echo "ci.sh: unknown option $1" >&2; usage ;;
    *) if [ -z "$build" ]; then build="$1"; else tests+=("$1"); fi ;;
  esac
  shift
done
[ -n "$build" ] && [ -d "$build" ] || usage
build="$(cd "$build" && pwd)"

# What the tests require here (unittest/cgroup_test_util.h, the MTR includes have_mle_*.inc).
export MLE_TEST_REQUIRE_CGROUP=1 MLE_TEST_REQUIRE_LANDLOCK=1 MLE_TEST_REQUIRE_WORKERS=1
scope() { systemd-run --user --scope -q -p Delegate=yes "$@"; }
if ! scope true 2>/dev/null; then
  echo "ci.sh: systemd-run --user --scope -p Delegate=yes does not work here: the workers' cgroup" \
       "limits cannot be tested (a user session of systemd is needed)" >&2
  exit 1
fi

failed=()
log="$build/ci-logs"
mkdir -p "$log"

if [ $do_build = 1 ]; then
  echo "== build"
  if ! cmake --build "$build" -j "$(nproc)" >"$log/build.log" 2>&1; then
    tail -30 "$log/build.log"
    echo "ci.sh: the build failed ($log/build.log)" >&2
    exit 1
  fi
fi

if [ $do_unit = 1 ]; then
  echo "== unit tests"
  for t in "$build"/runtime_output_directory/mle_*-t; do
    name="$(basename "$t")"
    # timeout outside the scope: the tests' cgroup must hold the test process alone.
    if timeout 1200 systemd-run --user --scope -q -p Delegate=yes "$t" --gtest_brief=1 >"$log/$name.log" 2>&1; then
      echo "   pass  $name"
    else
      echo "   FAIL  $name ($log/$name.log)"
      grep -E '^\[  FAILED  \] [A-Za-z]' "$log/$name.log" | sed 's/^/         /' | sort -u
      failed+=("$name")
    fi
  done
fi

if [ $do_unit = 1 ]; then
  # The worker's Java unit tests (JUnit; the build skips them, exp note 76 T7), with the build's JDK.
  java_home="$(sed -n 's/^MLE_JAVA_HOME:[A-Z]*=//p' "$build/CMakeCache.txt")"
  if JAVA_HOME="$java_home" mvn -q -B -f "$(dirname "$0")/worker/pom.xml" -Dmle.build.dir="$build/worker/maven" \
       test >"$log/worker-junit.log" 2>&1; then
    echo "   pass  worker JUnit ($(cat "$build"/worker/maven/surefire-reports/*.txt | grep -o 'Tests run: [0-9]*' \
      | awk '{s+=$3} END {print s}') tests)"
  else
    echo "   FAIL  worker JUnit ($log/worker-junit.log)"
    grep -E 'FAIL|Tests run:.*Fail' "$log/worker-junit.log" | head -5 | sed 's/^/         /'
    failed+=(worker-junit)
  fi
fi

if [ $do_mtr = 1 ]; then
  echo "== MySQL tests (component_mle)"
  if scope "$build/mtr" --force --parallel="$parallel" --max-test-fail=0 --retry=0 "${tests[@]}" \
       >"$log/mtr.log" 2>&1; then
    echo "   pass  $(grep -o 'All [0-9]* tests were successful' "$log/mtr.log")"
  else
    echo "   FAIL  MTR ($log/mtr.log)"
    grep -E '\[ (fail|skipped) \]' "$log/mtr.log" | sed 's/^/         /'
    failed+=(mtr)
  fi
fi

if [ ${#failed[@]} -gt 0 ]; then
  echo "ci.sh: FAILED: ${failed[*]}"
  exit 1
fi
echo "ci.sh: everything passed"
