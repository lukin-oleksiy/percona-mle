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
  )

FOREACH(test ${MLE_TESTS})
  MYSQL_ADD_EXECUTABLE(mle_${test}-t ${test}-t.cc
    ADD_TEST mle_${test}
    LINK_LIBRARIES mle_host gunit_small
    )
ENDFOREACH()
