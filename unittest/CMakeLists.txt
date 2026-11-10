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
