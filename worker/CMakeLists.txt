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
  SKIP_INSTALL
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
ADD_CUSTOM_TARGET(mle_worker_jar ALL DEPENDS ${MLE_WORKER_JAR})
SET_PROPERTY(GLOBAL PROPERTY MLE_WORKER_JAR ${MLE_WORKER_JAR})
