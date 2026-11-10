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

# The MLE JVM worker (D-10): mle_worker (C++ bootstrap: seccomp, then a JVM
# through JNI) and mle-worker.jar (Java, built with Maven).

SET(MLE_JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH
  "GraalVM JDK 25 for the MLE JVM worker")
IF(NOT EXISTS "${MLE_JAVA_HOME}/include/jni.h" OR
   NOT EXISTS "${MLE_JAVA_HOME}/lib/server/libjvm.so")
  MESSAGE(FATAL_ERROR
    "WITH_MLE: MLE_JAVA_HOME (${MLE_JAVA_HOME}) must be a GraalVM JDK 25 "
    "with include/jni.h and lib/server/libjvm.so")
ENDIF()
FIND_PROGRAM(MLE_MAVEN mvn)
IF(NOT MLE_MAVEN)
  MESSAGE(FATAL_ERROR "WITH_MLE: Maven (mvn) is needed to build mle-worker.jar")
ENDIF()

# libjvm is loaded with dlopen (path given at run time), so mle_worker does
# not depend on where the JDK is installed.
MYSQL_ADD_EXECUTABLE(mle_worker
  bootstrap/main.cc
  bootstrap/seccomp.cc
  LINK_LIBRARIES ${CMAKE_DL_LIBS}
  DESTINATION ${INSTALL_PLUGINDIR}/mle
  COMPONENT MleWorker
  )
TARGET_INCLUDE_DIRECTORIES(mle_worker SYSTEM PRIVATE
  ${MLE_JAVA_HOME}/include ${MLE_JAVA_HOME}/include/linux)

FILE(GLOB_RECURSE MLE_WORKER_JAVA_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main/java/*.java)
SET(MLE_WORKER_JAR ${CMAKE_CURRENT_BINARY_DIR}/maven/mle-worker.jar)
ADD_CUSTOM_COMMAND(OUTPUT ${MLE_WORKER_JAR}
  COMMAND ${CMAKE_COMMAND} -E env JAVA_HOME=${MLE_JAVA_HOME}
    ${MLE_MAVEN} -q -B -f ${CMAKE_CURRENT_SOURCE_DIR}/pom.xml
    -Dmle.build.dir=${CMAKE_CURRENT_BINARY_DIR}/maven package
  COMMAND ${CMAKE_COMMAND} -E touch ${MLE_WORKER_JAR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pom.xml ${MLE_WORKER_JAVA_SOURCES}
  COMMENT "Building mle-worker.jar with Maven"
  VERBATIM
  )

# GraalJS/GraalPy extract their resources (e.g. the Python standard library)
# into a cache on first use. Workers cannot write anywhere, so the cache is
# filled here, at build time, and workers read it
# (-Dpolyglot.engine.userResourceCache).
SET(MLE_RESOURCE_CACHE ${CMAKE_CURRENT_BINARY_DIR}/resources)
SET(MLE_RESOURCE_STAMP ${MLE_RESOURCE_CACHE}/.mle-setup-done)
ADD_CUSTOM_COMMAND(OUTPUT ${MLE_RESOURCE_STAMP}
  COMMAND ${MLE_JAVA_HOME}/bin/java
    -Dpolyglot.engine.userResourceCache=${MLE_RESOURCE_CACHE}
    --enable-native-access=ALL-UNNAMED
    -cp ${MLE_WORKER_JAR} com.percona.mle.worker.Setup
  COMMAND ${CMAKE_COMMAND} -E touch ${MLE_RESOURCE_STAMP}
  DEPENDS ${MLE_WORKER_JAR}
  COMMENT "Extracting GraalJS/GraalPy resources for MLE workers"
  VERBATIM
  )

# The GraalPy venv with numpy (D-6). The first build downloads the packages.
SET(MLE_PYTHON_VENV ${CMAKE_CURRENT_BINARY_DIR}/python-venv/venv)
SET(MLE_PYTHON_VENV_STAMP ${CMAKE_CURRENT_BINARY_DIR}/python-venv/.mle-venv-done)
ADD_CUSTOM_COMMAND(OUTPUT ${MLE_PYTHON_VENV_STAMP}
  COMMAND ${CMAKE_COMMAND} -E env JAVA_HOME=${MLE_JAVA_HOME}
    ${MLE_MAVEN} -q -B -f ${CMAKE_CURRENT_SOURCE_DIR}/python/pom.xml
    -Dmle.build.dir=${CMAKE_CURRENT_BINARY_DIR}/maven-python
    -Dmle.venv.dir=${CMAKE_CURRENT_BINARY_DIR}/python-venv
    generate-resources
  COMMAND ${CMAKE_COMMAND} -E touch ${MLE_PYTHON_VENV_STAMP}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/python/pom.xml
  COMMENT "Building the GraalPy venv for MLE workers (numpy)"
  VERBATIM
  )

# JDK AOT cache (JEP 483/515): a training run loads, links and profiles what
# a worker runs; workers then start from the cache. The JVM options that
# matter for the cache must match the worker's (defaults.cc).
SET(MLE_AOT_CACHE ${CMAKE_CURRENT_BINARY_DIR}/mle.aot)
SET(MLE_AOT_JVM_OPTIONS
  -XX:+UseSerialGC --enable-native-access=ALL-UNNAMED
  --add-modules=jdk.incubator.vector
  -Dpolyglot.engine.userResourceCache=${MLE_RESOURCE_CACHE}
  -Dmle.python.venv=${MLE_PYTHON_VENV}
  -cp ${MLE_WORKER_JAR})
ADD_CUSTOM_COMMAND(OUTPUT ${MLE_AOT_CACHE}
  COMMAND ${MLE_JAVA_HOME}/bin/java -XX:AOTMode=record
    -XX:AOTConfiguration=${CMAKE_CURRENT_BINARY_DIR}/mle.aotconf
    ${MLE_AOT_JVM_OPTIONS} com.percona.mle.worker.Train
  COMMAND ${MLE_JAVA_HOME}/bin/java -XX:AOTMode=create
    -XX:AOTConfiguration=${CMAKE_CURRENT_BINARY_DIR}/mle.aotconf
    -XX:AOTCache=${MLE_AOT_CACHE} ${MLE_AOT_JVM_OPTIONS}
  DEPENDS ${MLE_WORKER_JAR} ${MLE_RESOURCE_STAMP} ${MLE_PYTHON_VENV_STAMP}
  COMMENT "Training and creating the JDK AOT cache for MLE workers"
  VERBATIM
  )

ADD_CUSTOM_TARGET(mle_worker_jar ALL
  DEPENDS ${MLE_WORKER_JAR} ${MLE_RESOURCE_STAMP} ${MLE_PYTHON_VENV_STAMP}
          ${MLE_AOT_CACHE})
SET_PROPERTY(GLOBAL PROPERTY MLE_WORKER_JAR ${MLE_WORKER_JAR})
SET_PROPERTY(GLOBAL PROPERTY MLE_RESOURCE_CACHE ${MLE_RESOURCE_CACHE})
SET_PROPERTY(GLOBAL PROPERTY MLE_PYTHON_VENV ${MLE_PYTHON_VENV})
SET_PROPERTY(GLOBAL PROPERTY MLE_AOT_CACHE ${MLE_AOT_CACHE})

# Installed layout: <plugin dir>/mle (see include/mle/defaults.h). The
# component MleWorker is separate from Server for now (POC; packaging later).
# The AOT cache checks the jars' paths, sizes and modification times: CMake
# keeps the times when it installs, and the cache is installed with the jars.
# The venv is used only through its site-packages, so it can be moved.
SET(MLE_INSTALL_DIR ${INSTALL_PLUGINDIR}/mle)
INSTALL(FILES ${MLE_WORKER_JAR} ${MLE_AOT_CACHE}
  DESTINATION ${MLE_INSTALL_DIR} COMPONENT MleWorker)
INSTALL(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/maven/deps
                  ${MLE_RESOURCE_CACHE}
                  ${CMAKE_CURRENT_BINARY_DIR}/python-venv
  DESTINATION ${MLE_INSTALL_DIR} COMPONENT MleWorker USE_SOURCE_PERMISSIONS)
