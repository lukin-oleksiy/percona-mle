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

# The V8 worker (exp design note 63): LANGUAGE JS on the distribution's V8,
# which libnode exports (Debian, Ubuntu: libnode-dev, /usr/include/node and
# libv8.so -> libnode.so; RHEL family: nodejs-devel). V8 is never built here.
# -DMLE_V8=ON is the default when V8's headers and libnode are found.

FIND_PATH(MLE_V8_INCLUDE_DIR v8.h PATHS /usr/include/node /usr/local/include/node NO_DEFAULT_PATH)
FIND_LIBRARY(MLE_LIBNODE NAMES node)
SET(mle_v8_default OFF)
IF(MLE_V8_INCLUDE_DIR AND MLE_LIBNODE)
  SET(mle_v8_default ON)
ENDIF()
OPTION(MLE_V8 "Build the V8 worker (LANGUAGE JS) on the distribution's libnode" ${mle_v8_default})
IF(NOT MLE_V8)
  MESSAGE(STATUS "MLE V8 worker: off (no V8 headers in /usr/include/node or no libnode; libnode-dev)")
  RETURN()
ENDIF()
IF(NOT MLE_V8_INCLUDE_DIR OR NOT MLE_LIBNODE)
  MESSAGE(FATAL_ERROR "MLE_V8=ON needs V8's headers (/usr/include/node/v8.h) and libnode (libnode-dev)")
ENDIF()
FILE(STRINGS ${MLE_V8_INCLUDE_DIR}/v8-version.h mle_v8_major REGEX "^#define V8_MAJOR_VERSION")
FILE(STRINGS ${MLE_V8_INCLUDE_DIR}/v8-version.h mle_v8_minor REGEX "^#define V8_MINOR_VERSION")
STRING(REGEX MATCH "[0-9]+$" mle_v8_major "${mle_v8_major}")
STRING(REGEX MATCH "[0-9]+$" mle_v8_minor "${mle_v8_minor}")
GET_FILENAME_COMPONENT(mle_libnode_real ${MLE_LIBNODE} REALPATH)
GET_FILENAME_COMPONENT(MLE_LIBNODE_SONAME ${mle_libnode_real} NAME)
MESSAGE(STATUS "MLE V8 worker: V8 ${mle_v8_major}.${mle_v8_minor} from ${mle_libnode_real}")

# api.js and sql.js, compiled into the worker as strings.
SET(MLE_V8_JS_H)
FOREACH(js api sql)
  SET(h ${CMAKE_CURRENT_BINARY_DIR}/${js}_js.h)
  STRING(SUBSTRING ${js} 0 1 first)
  STRING(TOUPPER ${first} first)
  STRING(SUBSTRING ${js} 1 -1 rest)
  ADD_CUSTOM_COMMAND(OUTPUT ${h}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/${js}.js -DOUT=${h}
      -DNAME=k${first}${rest}Js -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${js}.js ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    VERBATIM)
  LIST(APPEND MLE_V8_JS_H ${h})
ENDFOREACH()

MYSQL_ADD_EXECUTABLE(mle_v8_worker
  main.cc
  worker.cc
  values.cc
  modules.cc
  ../common/mailbox.cc
  ../bootstrap/seccomp.cc
  ${MLE_V8_JS_H}
  LINK_LIBRARIES ${MLE_LIBNODE}
  DESTINATION ${INSTALL_PLUGINDIR}/mle/v8
  COMPONENT MleV8
  )
TARGET_INCLUDE_DIRECTORIES(mle_v8_worker PRIVATE
  ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/worker ${CMAKE_CURRENT_SOURCE_DIR}/../bootstrap
  ${CMAKE_CURRENT_BINARY_DIR})
TARGET_INCLUDE_DIRECTORIES(mle_v8_worker SYSTEM PRIVATE ${MLE_V8_INCLUDE_DIR})
# V8's headers: the build's flags as libnode's (no pointer compression, no sandbox: config.gypi).
TARGET_COMPILE_OPTIONS(mle_v8_worker PRIVATE -Wno-unused-parameter)

# The build tree's <plugin dir>/mle/v8 (CMakeLists.txt at the top links it).
SET(MLE_V8_DIR ${CMAKE_CURRENT_BINARY_DIR}/layout)
ADD_CUSTOM_TARGET(mle_v8_layout ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MLE_V8_DIR}
  COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE:mle_v8_worker> ${MLE_V8_DIR}/mle_v8_worker
  DEPENDS mle_v8_worker
  VERBATIM)
SET_PROPERTY(GLOBAL PROPERTY MLE_V8_DIR ${MLE_V8_DIR})
SET_PROPERTY(GLOBAL PROPERTY MLE_LIBNODE_SONAME ${MLE_LIBNODE_SONAME})
