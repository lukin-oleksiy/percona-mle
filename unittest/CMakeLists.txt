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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

IF(NOT WITH_MLE)
  RETURN()
ENDIF()

INCLUDE_DIRECTORIES(SYSTEM ${GMOCK_INCLUDE_DIRS})

SET(MLE_TESTS
  codec
  codec_fuzz
  launcher
  java_worker
  worker_protocol
  polyglot_worker
  worker_failures
  )

FOREACH(test ${MLE_TESTS})
  MYSQL_ADD_EXECUTABLE(mle_${test}-t ${test}-t.cc
    ADD_TEST mle_${test}
    LINK_LIBRARIES mle_host gunit_small
    )
ENDFOREACH()

# A minimal worker (no GraalVM) for the launcher tests.
MYSQL_ADD_EXECUTABLE(mle_test_worker mle_test_worker.cc
  LINK_LIBRARIES mle_host
  SKIP_INSTALL
  )
TARGET_COMPILE_DEFINITIONS(mle_launcher-t PRIVATE
  MLE_LAUNCHER_EXE="$<TARGET_FILE:mle_launcher>"
  MLE_TEST_WORKER_EXE="$<TARGET_FILE:mle_test_worker>"
  )
ADD_DEPENDENCIES(mle_launcher-t mle_launcher mle_test_worker)
TARGET_COMPILE_DEFINITIONS(mle_worker_protocol-t PRIVATE
  MLE_LAUNCHER_EXE="$<TARGET_FILE:mle_launcher>"
  MLE_TEST_WORKER_EXE="$<TARGET_FILE:mle_test_worker>"
  )
ADD_DEPENDENCIES(mle_worker_protocol-t mle_launcher mle_test_worker)

# A GraalPy venv with numpy for the Python tests (optional; numpy tests are
# skipped without it).
SET(MLE_TEST_PYTHON_VENV "" CACHE PATH "GraalPy venv with numpy for the MLE tests")
GET_PROPERTY(MLE_WORKER_JAR GLOBAL PROPERTY MLE_WORKER_JAR)
GET_PROPERTY(MLE_RESOURCE_CACHE GLOBAL PROPERTY MLE_RESOURCE_CACHE)
FOREACH(test java_worker polyglot_worker worker_failures)
  TARGET_COMPILE_DEFINITIONS(mle_${test}-t PRIVATE
    MLE_LAUNCHER_EXE="$<TARGET_FILE:mle_launcher>"
    MLE_WORKER_EXE="$<TARGET_FILE:mle_worker>"
    MLE_WORKER_JAR="${MLE_WORKER_JAR}"
    MLE_RESOURCE_CACHE="${MLE_RESOURCE_CACHE}"
    MLE_JAVA_HOME="${MLE_JAVA_HOME}"
    MLE_TEST_PYTHON_VENV="${MLE_TEST_PYTHON_VENV}"
    )
  ADD_DEPENDENCIES(mle_${test}-t mle_launcher mle_worker mle_worker_jar)
ENDFOREACH()
