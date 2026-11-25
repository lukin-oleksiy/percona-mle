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

# The CPython worker (exp design note 62): mle.worker = CPYTHON runs Python
# routines on the distribution's CPython. Installed as <plugin dir>/mle/cpython:
#   mle_cpython_worker   the executable (seccomp, embedded CPython, _mlewire)
#   python/              the worker's Python code (package mle_cpython)
#   site-packages/       numpy, installed at build time

FIND_PACKAGE(Python3 3.12 COMPONENTS Interpreter Development.Embed)
IF(Python3_FOUND)
  SET(MLE_CPYTHON_DEFAULT ON)
ELSE()
  SET(MLE_CPYTHON_DEFAULT OFF)
ENDIF()
OPTION(MLE_CPYTHON "Build the CPython worker (Python 3.12 or later with its headers)"
  ${MLE_CPYTHON_DEFAULT})
IF(NOT MLE_CPYTHON)
  IF(MLE_REQUIRE_ALL_WORKERS)
    MESSAGE(FATAL_ERROR "MLE_REQUIRE_ALL_WORKERS: the CPython worker needs Python 3.12 or later "
      "with its development files (python3-dev, python3-devel)")
  ENDIF()
  MESSAGE(STATUS "MLE CPython worker: off (no Python 3.12 or later with its development files): "
    "no percona-mle-cpython package")
  RETURN()
ENDIF()
IF(NOT Python3_FOUND)
  MESSAGE(FATAL_ERROR "MLE_CPYTHON needs Python 3.12 or later with its development "
    "files (python3-dev, python3-devel)")
ENDIF()
SET(MLE_CPYTHON_NUMPY "numpy==2.4.4" CACHE STRING
  "The numpy the CPython worker's site-packages get (pip requirement)")

# The standard library this worker embeds: fixed at build time, as the
# package depends on this Python version.
EXECUTE_PROCESS(
  COMMAND ${Python3_EXECUTABLE} -c
    "import sysconfig; print(sysconfig.get_path('stdlib')); print(sysconfig.get_config_var('DESTSHARED')); print(sysconfig.get_config_var('INSTSONAME'))"
  OUTPUT_VARIABLE MLE_PY_PATHS OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE MLE_PY_RESULT)
IF(NOT MLE_PY_RESULT EQUAL 0)
  MESSAGE(FATAL_ERROR "MLE: cannot ask ${Python3_EXECUTABLE} for its standard library")
ENDIF()
STRING(REPLACE "\n" ";" MLE_PY_PATHS "${MLE_PY_PATHS}")
LIST(GET MLE_PY_PATHS 0 MLE_PYTHON_STDLIB)
LIST(GET MLE_PY_PATHS 1 MLE_PYTHON_DYNLOAD)
LIST(GET MLE_PY_PATHS 2 MLE_PYTHON_SONAME)
MESSAGE(STATUS "MLE CPython worker: Python ${Python3_VERSION}, ${MLE_PYTHON_STDLIB}")

MYSQL_ADD_EXECUTABLE(mle_cpython_worker
  main.cc
  mailbox.cc
  module.cc
  codec.cc
  ../common/mailbox.cc
  ../common/result.cc
  ../bootstrap/seccomp.cc
  LINK_LIBRARIES Python3::Python
  DESTINATION ${INSTALL_PLUGINDIR}/mle/cpython
  COMPONENT MleCPython
  )
TARGET_INCLUDE_DIRECTORIES(mle_cpython_worker PRIVATE
  ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/worker ${CMAKE_CURRENT_SOURCE_DIR}/../bootstrap)
TARGET_INCLUDE_DIRECTORIES(mle_cpython_worker SYSTEM PRIVATE ${Python3_INCLUDE_DIRS})
TARGET_COMPILE_DEFINITIONS(mle_cpython_worker PRIVATE
  MLE_PYTHON_STDLIB="${MLE_PYTHON_STDLIB}"
  MLE_PYTHON_DYNLOAD="${MLE_PYTHON_DYNLOAD}")

# The build tree's <plugin dir>/mle/cpython (CMakeLists.txt at the top links
# it): the executable, the Python code (a link to the sources: an edit needs
# no build, a new worker reads it; without bytecode, as workers cannot write
# it) and site-packages. Installed, the code is a copy with its bytecode.
SET(MLE_CPYTHON_DIR ${CMAKE_CURRENT_BINARY_DIR}/layout)
# The Python version, for mle_package (packages must be built for it).
FILE(WRITE ${MLE_CPYTHON_DIR}/python-version "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}\n")
# numpy from PyPI through a build venv (the distribution's pip refuses to
# install into the system Python, PEP 668), into a plain directory.
SET(MLE_CPYTHON_SITE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/.mle-cpython-site)
ADD_CUSTOM_COMMAND(OUTPUT ${MLE_CPYTHON_SITE_STAMP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${MLE_CPYTHON_DIR}/site-packages ${CMAKE_CURRENT_BINARY_DIR}/build-venv
  COMMAND ${Python3_EXECUTABLE} -m venv ${CMAKE_CURRENT_BINARY_DIR}/build-venv
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/build-venv/bin/pip install -q --disable-pip-version-check
    --only-binary=:all: --target ${MLE_CPYTHON_DIR}/site-packages ${MLE_CPYTHON_NUMPY}
  # pip's console scripts name the build venv's interpreter; workers run none.
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${MLE_CPYTHON_DIR}/site-packages/bin
  COMMAND ${CMAKE_COMMAND} -E touch ${MLE_CPYTHON_SITE_STAMP}
  COMMENT "Installing ${MLE_CPYTHON_NUMPY} for the CPython worker"
  VERBATIM
  )
ADD_CUSTOM_TARGET(mle_cpython_layout ALL
  COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE:mle_cpython_worker>
    ${MLE_CPYTHON_DIR}/mle_cpython_worker
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${MLE_CPYTHON_DIR}/python
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_SOURCE_DIR}/python ${MLE_CPYTHON_DIR}/python
  # Bytecode of the linked sources outside the source tree (the worker's --pycache): workers
  # start without compiling; after an edit Python compiles the changed module in memory.
  COMMAND ${CMAKE_COMMAND} -E env PYTHONPYCACHEPREFIX=${MLE_CPYTHON_DIR}/pycache
    ${Python3_EXECUTABLE} -m compileall -q ${CMAKE_CURRENT_SOURCE_DIR}/python
  DEPENDS ${MLE_CPYTHON_SITE_STAMP}
  VERBATIM
  )
ADD_DEPENDENCIES(mle_cpython_layout mle_cpython_worker)
SET_PROPERTY(GLOBAL PROPERTY MLE_CPYTHON_DIR ${MLE_CPYTHON_DIR})
SET_PROPERTY(GLOBAL PROPERTY MLE_PYTHON_STDLIB ${MLE_PYTHON_STDLIB})
SET_PROPERTY(GLOBAL PROPERTY MLE_PYTHON_DYNLOAD ${MLE_PYTHON_DYNLOAD})
# For the package's dependency (cmake/packaging.cmake).
SET_PROPERTY(GLOBAL PROPERTY MLE_PYTHON_SONAME ${MLE_PYTHON_SONAME})
SET_PROPERTY(GLOBAL PROPERTY MLE_PYTHON_MAJOR_MINOR "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")

INSTALL(FILES ${MLE_CPYTHON_DIR}/python-version
  DESTINATION ${INSTALL_PLUGINDIR}/mle/cpython COMPONENT MleCPython)
# Read-only for others whatever the builder's umask (pip and git keep it), as the JVM worker's
# files (MVP review OPS-12); shared objects keep their execute bits.
INSTALL(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python ${MLE_CPYTHON_DIR}/site-packages
  DESTINATION ${INSTALL_PLUGINDIR}/mle/cpython COMPONENT MleCPython
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
  DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
  PATTERN __pycache__ EXCLUDE
  REGEX "\\.so(\\.[0-9.]+)?$"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
# Bytecode for the installed code (workers cannot write it); umask 022 for its directories too.
INSTALL(CODE "EXECUTE_PROCESS(COMMAND sh -c \"umask 022 && exec \\\"$0\\\" -m compileall -q \\\"$1\\\"\"
  ${Python3_EXECUTABLE} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${INSTALL_PLUGINDIR}/mle/cpython/python\")"
  COMPONENT MleCPython)
