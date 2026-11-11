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

# MLE: GraalVM-based multi-language stored programs, run out of process
# (worker processes started by a launcher). Linux only.
OPTION(WITH_MLE "Build the MLE (multi-language engine) host library, launcher and worker" OFF)

IF(NOT WITH_MLE)
  RETURN()
ENDIF()
IF(NOT LINUX)
  MESSAGE(FATAL_ERROR "WITH_MLE is supported on Linux only")
ENDIF()
MESSAGE(STATUS "Building MLE")

# Installed MLE directories are not writable by the group or others, whatever
# the builder's umask: workers run what is in them.
SET(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE)
ADD_SUBDIRECTORY(host)
ADD_SUBDIRECTORY(launcher)
ADD_SUBDIRECTORY(worker)
ADD_SUBDIRECTORY(harness)

# The build tree gets the installed layout (include/mle/defaults.h) as
# symlinks in <build>/plugin_output_directory/mle, so component_mle finds
# its files there with the default mle.dir (<plugin dir>/mle), e.g. in MTR.
GET_PROPERTY(MLE_WORKER_JAR GLOBAL PROPERTY MLE_WORKER_JAR)
GET_PROPERTY(MLE_RESOURCE_CACHE GLOBAL PROPERTY MLE_RESOURCE_CACHE)
GET_PROPERTY(MLE_PYTHON_VENV GLOBAL PROPERTY MLE_PYTHON_VENV)
GET_PROPERTY(MLE_AOT_CACHE GLOBAL PROPERTY MLE_AOT_CACHE)
GET_FILENAME_COMPONENT(MLE_PYTHON_VENV_PARENT ${MLE_PYTHON_VENV} DIRECTORY)
SET(MLE_BUILD_LAYOUT ${CMAKE_BINARY_DIR}/plugin_output_directory/mle)
ADD_CUSTOM_TARGET(mle_build_layout ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MLE_BUILD_LAYOUT}
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE:mle_launcher> ${MLE_BUILD_LAYOUT}/mle_launcher
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    $<TARGET_FILE:mle_worker> ${MLE_BUILD_LAYOUT}/mle_worker
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${MLE_WORKER_JAR} ${MLE_BUILD_LAYOUT}/mle-worker.jar
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${MLE_RESOURCE_CACHE} ${MLE_BUILD_LAYOUT}/resources
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${MLE_PYTHON_VENV_PARENT} ${MLE_BUILD_LAYOUT}/python-venv
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${MLE_AOT_CACHE} ${MLE_BUILD_LAYOUT}/mle.aot
  COMMAND ${CMAKE_COMMAND} -E create_symlink
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mle_package ${MLE_BUILD_LAYOUT}/mle_package
  COMMENT "Linking the MLE layout into plugin_output_directory/mle"
  VERBATIM
  )
ADD_DEPENDENCIES(mle_build_layout mle_launcher mle_worker mle_worker_jar)

# The DBA's tool for third-party packages (14-phase6-libraries.md §3).
INSTALL(PROGRAMS tools/mle_package DESTINATION ${INSTALL_PLUGINDIR}/mle
  COMPONENT MleWorker)

ADD_SUBDIRECTORY(component)
