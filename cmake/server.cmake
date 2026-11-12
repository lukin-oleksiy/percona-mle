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

# Building against Percona Server (22-separate-product.md §3): the server's
# headers from a source tree and a configured build of it (generated
# headers), the few server CMake macros the MLE directories use, in plain
# CMake, and the server's compile settings that its headers depend on.
#
#   cmake -DMYSQL_SOURCE_DIR=<server source> -DMYSQL_BUILD_DIR=<server build>

SET(MYSQL_SOURCE_DIR "" CACHE PATH "Percona Server source tree")
SET(MYSQL_BUILD_DIR "" CACHE PATH
  "A configured build of MYSQL_SOURCE_DIR (generated headers; MTR, GoogleTest)")
FOREACH(f ${MYSQL_SOURCE_DIR}/include/mysql/components/services/mysql_stored_program.h
          ${MYSQL_BUILD_DIR}/include/mysql_version.h
          ${MYSQL_BUILD_DIR}/include/mysqld_error.h)
  IF(NOT EXISTS ${f})
    MESSAGE(FATAL_ERROR "Percona MLE builds against Percona Server: "
      "-DMYSQL_SOURCE_DIR=<server source> -DMYSQL_BUILD_DIR=<configured server "
      "build> (missing ${f})")
  ENDIF()
ENDFOREACH()
IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  MESSAGE(FATAL_ERROR "Percona MLE is supported on Linux only")
ENDIF()
SET(LINUX 1)

# The server line. Supported: trunk-based releases (26.x), then 9.7.
FILE(STRINGS ${MYSQL_BUILD_DIR}/include/mysql_version.h MYSQL_VERSION_LINE
  REGEX "^#define MYSQL_VERSION_ID[ \t]+[0-9]+")
STRING(REGEX REPLACE ".*[ \t]([0-9]+)$" "\\1" MYSQL_VERSION_ID "${MYSQL_VERSION_LINE}")
IF(MYSQL_VERSION_ID LESS 90700)
  MESSAGE(FATAL_ERROR "Percona MLE needs Percona Server 9.7 or a trunk-based "
    "release (26.x); ${MYSQL_BUILD_DIR} is ${MYSQL_VERSION_ID}")
ENDIF()
MESSAGE(STATUS "Percona MLE for server version ${MYSQL_VERSION_ID} (${MYSQL_BUILD_DIR})")

# The server build's settings: build type (debug server: debug MLE) and the
# install layout the packages use.
LOAD_CACHE(${MYSQL_BUILD_DIR} READ_WITH_PREFIX SERVER_
  CMAKE_BUILD_TYPE CMAKE_INSTALL_PREFIX INSTALL_PLUGINDIR)
IF(NOT CMAKE_BUILD_TYPE)
  SET(CMAKE_BUILD_TYPE "${SERVER_CMAKE_BUILD_TYPE}" CACHE STRING "Build type (default: the server's)" FORCE)
ENDIF()
STRING(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
IF(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  SET(CMAKE_INSTALL_PREFIX "${SERVER_CMAKE_INSTALL_PREFIX}" CACHE PATH "Install prefix (default: the server's)" FORCE)
ENDIF()
SET(INSTALL_PLUGINDIR "${SERVER_INSTALL_PLUGINDIR}" CACHE STRING "Plugin directory below the prefix (default: the server's)")

# As the server compiles its components (cmake/component.cmake and the
# server's global flags that its headers depend on).
SET(CMAKE_CXX_STANDARD 23)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS ON)
SET(CMAKE_POSITION_INDEPENDENT_CODE ON)
ADD_COMPILE_DEFINITIONS(_FILE_OFFSET_BITS=64 _GNU_SOURCE __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS)
IF(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
  ADD_COMPILE_DEFINITIONS(SAFE_MUTEX ENABLED_DEBUG_SYNC)
ELSE()
  ADD_COMPILE_DEFINITIONS(NDEBUG)
ENDIF()
ADD_COMPILE_OPTIONS(-fno-omit-frame-pointer -Wall -Wextra -Wformat-security -Wvla
  -Wundef -Woverloaded-virtual -Wimplicit-fallthrough=5 -Wsuggest-override -Wextra-semi)
INCLUDE_DIRECTORIES(${MYSQL_BUILD_DIR} ${MYSQL_BUILD_DIR}/include
  ${MYSQL_SOURCE_DIR} ${MYSQL_SOURCE_DIR}/include ${MYSQL_SOURCE_DIR}/libs)

# Outputs where the server's build has them: the layout code and tests
# expect <build>/plugin_output_directory and <build>/runtime_output_directory.
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime_output_directory)
SET(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/archive_output_directory)

# MYSQL_ADD_COMPONENT(name sources... [LINK_LIBRARIES libs...] [MODULE_ONLY]
#                     [SKIP_INSTALL] [TEST_ONLY]): component_<name>.so
MACRO(MYSQL_ADD_COMPONENT name)
  CMAKE_PARSE_ARGUMENTS(ARG "MODULE_ONLY;SKIP_INSTALL;TEST_ONLY" "" "LINK_LIBRARIES" ${ARGN})
  SET(target component_${name})
  ADD_LIBRARY(${target} MODULE ${ARG_UNPARSED_ARGUMENTS})
  TARGET_COMPILE_DEFINITIONS(${target} PUBLIC MYSQL_COMPONENT)
  IF(ARG_LINK_LIBRARIES)
    TARGET_LINK_LIBRARIES(${target} ${ARG_LINK_LIBRARIES})
  ENDIF()
  SET_TARGET_PROPERTIES(${target} PROPERTIES
    PREFIX "" OUTPUT_NAME ${target}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugin_output_directory
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  IF(NOT ARG_SKIP_INSTALL)
    INSTALL(TARGETS ${target} DESTINATION ${INSTALL_PLUGINDIR} COMPONENT MleCore)
  ENDIF()
ENDMACRO()

# MYSQL_ADD_EXECUTABLE(target sources... [LINK_LIBRARIES libs...]
#   [DESTINATION dir] [COMPONENT c] [ADD_TEST name] [SKIP_INSTALL])
FUNCTION(MYSQL_ADD_EXECUTABLE target)
  CMAKE_PARSE_ARGUMENTS(ARG "SKIP_INSTALL" "DESTINATION;COMPONENT;ADD_TEST" "LINK_LIBRARIES" ${ARGN})
  ADD_EXECUTABLE(${target} ${ARG_UNPARSED_ARGUMENTS})
  IF(ARG_LINK_LIBRARIES)
    TARGET_LINK_LIBRARIES(${target} ${ARG_LINK_LIBRARIES})
  ENDIF()
  IF(ARG_ADD_TEST)
    ADD_TEST(NAME ${ARG_ADD_TEST} COMMAND ${target})
  ENDIF()
  IF(ARG_DESTINATION AND NOT ARG_SKIP_INSTALL)
    INSTALL(TARGETS ${target} DESTINATION ${ARG_DESTINATION} COMPONENT ${ARG_COMPONENT})
  ENDIF()
ENDFUNCTION()

# MYSQL_INSTALL_TARGET(target DESTINATION dir COMPONENT c)
FUNCTION(MYSQL_INSTALL_TARGET target)
  CMAKE_PARSE_ARGUMENTS(ARG "" "DESTINATION;COMPONENT" "" ${ARGN})
  INSTALL(TARGETS ${target} DESTINATION ${ARG_DESTINATION} COMPONENT ${ARG_COMPONENT})
ENDFUNCTION()

# INSTALL_DEBUG_TARGET(target DESTINATION dir COMPONENT c): the target of
# the debug build MLE_DEBUG_BUILD_DIR (a Debug build of percona-mle against a
# debug server), if it has it.
FUNCTION(INSTALL_DEBUG_TARGET target)
  CMAKE_PARSE_ARGUMENTS(ARG "" "DESTINATION;COMPONENT" "" ${ARGN})
  GET_TARGET_PROPERTY(type ${target} TYPE)
  IF(type STREQUAL "MODULE_LIBRARY")
    SET(file ${MLE_DEBUG_BUILD_DIR}/plugin_output_directory/${target}.so)
  ELSE()
    SET(file ${MLE_DEBUG_BUILD_DIR}/runtime_output_directory/${target})
  ENDIF()
  INSTALL(PROGRAMS ${file} DESTINATION ${ARG_DESTINATION} COMPONENT ${ARG_COMPONENT} OPTIONAL)
ENDFUNCTION()

# GoogleTest for the unit tests: the one the server build made (as gunit).
FILE(GLOB MLE_GTEST_SOURCE_DIR LIST_DIRECTORIES true ${MYSQL_SOURCE_DIR}/extra/googletest/googletest-*)
SET(MLE_GTEST_INCLUDE_DIRS ${MLE_GTEST_SOURCE_DIR}/googlemock/include
  ${MLE_GTEST_SOURCE_DIR}/googletest/include)
FIND_LIBRARY(MLE_GTEST_LIB gtest PATHS ${MYSQL_BUILD_DIR} NO_DEFAULT_PATH)
FIND_LIBRARY(MLE_GTEST_MAIN_LIB gtest_main PATHS ${MYSQL_BUILD_DIR} NO_DEFAULT_PATH)
FIND_LIBRARY(MLE_GMOCK_LIB gmock PATHS ${MYSQL_BUILD_DIR} NO_DEFAULT_PATH)
