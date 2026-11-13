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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Packages of Percona MLE (CPack: `cpack -G DEB` / `cpack -G RPM` in the build directory), one per
# install component. The paths follow the server build's install layout: build the server with
# INSTALL_LAYOUT=DEB or RPM for packages (a standalone server build gives <prefix>/lib/plugin).
#
#   percona-mle-common         component_mle, mle_launcher, mle_worker            both builds
#   percona-mle                the JVM build: worker jar, runtime jars, AOT cache  -> common, jdk
#   percona-mle-jdk            the trimmed GraalVM runtime (-DMLE_BUILD_JDK=ON)
#   percona-mle-python-runtime Python's standard library and the numpy venv (both builds) -> common
#   percona-mle-python         GraalPy for the JVM build (jars)                      -> percona-mle, runtime
#   percona-mle-python-native  the Python-only native build (-DMLE_PYTHON_NATIVE=ON) -> common, runtime
#   percona-mle-tools          mle_package (third-party packages; uses the JVM build)
#
# mle.worker = AUTO runs the JVM build when percona-mle is installed, else the native one.

SET(MLE_VERSION "0.1.0" CACHE STRING "Version of the Percona MLE packages")
SET(MLE_PACKAGE_RELEASE "1" CACHE STRING "Release of the Percona MLE packages")

# The server version the component was built against: the packages need that server line.
FILE(STRINGS ${MYSQL_BUILD_DIR}/include/mysql_version.h MLE_SERVER_VERSION_LINE
  REGEX "^#define MYSQL_SERVER_VERSION[ \t]+\"")
STRING(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" MLE_SERVER_VERSION
  "${MLE_SERVER_VERSION_LINE}")

SET(CPACK_PACKAGE_NAME percona-mle)
SET(CPACK_PACKAGE_VENDOR "Percona")
SET(CPACK_PACKAGE_CONTACT "Percona <info@percona.com>")
SET(CPACK_PACKAGE_VERSION ${MLE_VERSION})
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Percona MLE: Python, Java and JavaScript stored programs for Percona Server")
SET(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
SET(CPACK_COMPONENTS_ALL MleCommon MleCore MleJdk MlePythonRuntime MlePython MlePythonNative MleTools)
SET(CPACK_DEB_COMPONENT_INSTALL ON)
SET(CPACK_RPM_COMPONENT_INSTALL ON)
SET(CPACK_COMPONENTS_GROUPING IGNORE)

# Debian
SET(CPACK_DEBIAN_PACKAGE_RELEASE ${MLE_PACKAGE_RELEASE})
SET(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
SET(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
SET(CPACK_DEBIAN_PACKAGE_SECTION database)
# The runtime is prebuilt (JDK, numpy venv, a Native Image): no shlibdeps over it (21 §5).
SET(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
SET(mle_deb_ver "${MLE_VERSION}-${MLE_PACKAGE_RELEASE}")
SET(CPACK_DEBIAN_MLECOMMON_PACKAGE_NAME percona-mle-common)
SET(CPACK_DEBIAN_MLECOMMON_PACKAGE_DEPENDS
  "percona-server-server-core (>= ${MLE_SERVER_VERSION}), libc6, libstdc++6, libgcc-s1")
SET(CPACK_DEBIAN_MLECORE_PACKAGE_NAME percona-mle)
SET(CPACK_DEBIAN_MLECORE_PACKAGE_DEPENDS
  "percona-mle-common (= ${mle_deb_ver}), percona-mle-jdk (= ${mle_deb_ver}), zlib1g")
SET(CPACK_DEBIAN_MLECORE_PACKAGE_CONTROL_EXTRA
  "${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/postinst;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/prerm;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/triggers")
SET(CPACK_DEBIAN_MLEJDK_PACKAGE_NAME percona-mle-jdk)
SET(CPACK_DEBIAN_MLEPYTHONRUNTIME_PACKAGE_NAME percona-mle-python-runtime)
SET(CPACK_DEBIAN_MLEPYTHONRUNTIME_PACKAGE_DEPENDS "percona-mle-common (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLEPYTHON_PACKAGE_NAME percona-mle-python)
SET(CPACK_DEBIAN_MLEPYTHON_PACKAGE_DEPENDS
  "percona-mle (= ${mle_deb_ver}), percona-mle-python-runtime (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLEPYTHONNATIVE_PACKAGE_NAME percona-mle-python-native)
SET(CPACK_DEBIAN_MLEPYTHONNATIVE_PACKAGE_DEPENDS
  "percona-mle-common (= ${mle_deb_ver}), percona-mle-python-runtime (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLEPYTHONNATIVE_DESCRIPTION
  "Percona MLE: the Python-only native build (highly optimised for Python-only use: less memory, fast worker start)")
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_NAME percona-mle-tools)
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_DEPENDS "percona-mle (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_RECOMMENDS "nodejs, npm, maven")

# RPM (to be checked on RHEL)
SET(CPACK_RPM_PACKAGE_RELEASE ${MLE_PACKAGE_RELEASE})
SET(CPACK_RPM_FILE_NAME RPM-DEFAULT)
SET(CPACK_RPM_PACKAGE_LICENSE "GPLv2")
SET(CPACK_RPM_PACKAGE_ARCHITECTURE x86_64)
SET(CPACK_RPM_PACKAGE_AUTOREQPROV OFF)
# The server's directories belong to the server's package, not to ours.
SET(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/lib64/mysql /usr/lib64/mysql/plugin /usr/lib/mysql /usr/lib/mysql/plugin)
# Prebuilt runtime: no stripping, debuginfo or Python byte-compiling of the venv by rpmbuild.
SET(CPACK_RPM_SPEC_MORE_DEFINE "%define __strip /bin/true
%define __os_install_post %{nil}
%define _build_id_links none
%define debug_package %{nil}")
SET(mle_rpm_ver "${MLE_VERSION}-${MLE_PACKAGE_RELEASE}")
SET(CPACK_RPM_MLECOMMON_PACKAGE_NAME percona-mle-common)
SET(CPACK_RPM_MLECOMMON_PACKAGE_REQUIRES "percona-server-server >= ${MLE_SERVER_VERSION}")
SET(CPACK_RPM_MLECORE_PACKAGE_NAME percona-mle)
SET(CPACK_RPM_MLECORE_PACKAGE_REQUIRES
  "percona-mle-common = ${mle_rpm_ver}, percona-mle-jdk = ${mle_rpm_ver}")
SET(CPACK_RPM_MLECORE_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle.post)
SET(CPACK_RPM_MLECORE_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle.preun)
SET(CPACK_RPM_MLEJDK_PACKAGE_NAME percona-mle-jdk)
SET(CPACK_RPM_MLEPYTHONRUNTIME_PACKAGE_NAME percona-mle-python-runtime)
SET(CPACK_RPM_MLEPYTHONRUNTIME_PACKAGE_REQUIRES "percona-mle-common = ${mle_rpm_ver}")
SET(CPACK_RPM_MLEPYTHON_PACKAGE_NAME percona-mle-python)
SET(CPACK_RPM_MLEPYTHON_PACKAGE_REQUIRES
  "percona-mle = ${mle_rpm_ver}, percona-mle-python-runtime = ${mle_rpm_ver}")
SET(CPACK_RPM_MLEPYTHONNATIVE_PACKAGE_NAME percona-mle-python-native)
SET(CPACK_RPM_MLEPYTHONNATIVE_PACKAGE_REQUIRES
  "percona-mle-common = ${mle_rpm_ver}, percona-mle-python-runtime = ${mle_rpm_ver}")
SET(CPACK_RPM_MLETOOLS_PACKAGE_NAME percona-mle-tools)
SET(CPACK_RPM_MLETOOLS_PACKAGE_REQUIRES "percona-mle = ${mle_rpm_ver}")

INCLUDE(CPack)
