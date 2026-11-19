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
#   percona-mle-common         component_mle, mle_launcher, mle_worker            every worker
#   percona-mle-apparmor       the AppArmor profile (DEB only; -DMLE_APPARMOR=ON, the
#                              default where /etc/apparmor.d exists)               <- common
#   percona-mle                the JVM build: worker jar, runtime jars, AOT cache  -> common, jdk
#   percona-mle-jdk            the trimmed GraalVM runtime (-DMLE_BUILD_JDK=ON; without it no
#                              such package, and percona-mle does not ask for one)
#   percona-mle-python-runtime GraalPy's standard library and the numpy venv     -> common
#   percona-mle-python         GraalPy for the JVM build (jars)                      -> percona-mle, runtime
#   percona-mle-cpython        the CPython worker (-DMLE_CPYTHON=ON, the default with Python 3.12+;
#                              exp design note 62): Python only, on the distribution's
#                              libpython, with numpy (LANGUAGE CPYTHON)              -> common, libpython
#   percona-mle-v8             the V8 worker (-DMLE_V8=ON, the default with libnode's V8 headers;
#                              exp design note 63): LANGUAGE JS on the distribution's
#                              libnode (its V8)                                      -> common, libnode
#   percona-mle-tools          mle_package (third-party packages; uses the JVM build)
#   percona-mle-selinux        the SELinux policy module (RPM only; -DMLE_SELINUX=ON, the default
#                              with selinux-policy-devel)                          <- common
#
# Every installed worker runs its languages (exp design note 63): the JVM worker JAVASCRIPT, JAVA
# and PYTHON, the CPython worker CPYTHON.

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
SET(CPACK_COMPONENTS_ALL MleCommon MleCore MlePythonRuntime MlePython MleTools)
# Only the components the build has install rules for (both options are OFF by default).
IF(MLE_BUILD_JDK)
  LIST(APPEND CPACK_COMPONENTS_ALL MleJdk)
  SET(mle_deb_jdk ", percona-mle-jdk (= ${MLE_VERSION}-${MLE_PACKAGE_RELEASE})")
  SET(mle_rpm_jdk ", percona-mle-jdk = ${MLE_VERSION}-${MLE_PACKAGE_RELEASE}")
ELSE()
  SET(mle_deb_jdk "")
  SET(mle_rpm_jdk "")
  MESSAGE(STATUS "MLE packages: no percona-mle-jdk (-DMLE_BUILD_JDK=OFF); with such packages "
    "mle.java_home must name a GraalVM JDK 25 on the host they are installed on")
ENDIF()
GET_PROPERTY(MLE_PYTHON_SONAME GLOBAL PROPERTY MLE_PYTHON_SONAME)
GET_PROPERTY(MLE_PYTHON_MAJOR_MINOR GLOBAL PROPERTY MLE_PYTHON_MAJOR_MINOR)
IF(MLE_PYTHON_SONAME)
  LIST(APPEND CPACK_COMPONENTS_ALL MleCPython)
ENDIF()
GET_PROPERTY(MLE_LIBNODE_SONAME GLOBAL PROPERTY MLE_LIBNODE_SONAME)
IF(MLE_LIBNODE_SONAME)
  LIST(APPEND CPACK_COMPONENTS_ALL MleV8)
  # libnode.so.127 -> Debian's package libnode127, the RPM capability libnode.so.127()(64bit)
  STRING(REGEX REPLACE "^lib(node)\\.so\\.([0-9]+)$" "lib\\1\\2" mle_libnode_deb "${MLE_LIBNODE_SONAME}")
ENDIF()
IF(MLE_SELINUX)
  LIST(APPEND CPACK_COMPONENTS_ALL MleSelinux)
ENDIF()
IF(MLE_APPARMOR)
  LIST(APPEND CPACK_COMPONENTS_ALL MleApparmor)
ENDIF()
SET(CPACK_PROJECT_CONFIG_FILE ${CMAKE_SOURCE_DIR}/cmake/cpack_project_config.cmake)
SET(CPACK_DEB_COMPONENT_INSTALL ON)
SET(CPACK_RPM_COMPONENT_INSTALL ON)
SET(CPACK_COMPONENTS_GROUPING IGNORE)

# Debian
SET(CPACK_DEBIAN_PACKAGE_RELEASE ${MLE_PACKAGE_RELEASE})
SET(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
SET(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
SET(CPACK_DEBIAN_PACKAGE_SECTION database)
# The runtime is prebuilt (JDK, numpy venv): no shlibdeps over it (21 §5).
SET(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
SET(mle_deb_ver "${MLE_VERSION}-${MLE_PACKAGE_RELEASE}")
SET(CPACK_DEBIAN_MLECOMMON_PACKAGE_NAME percona-mle-common)
SET(CPACK_DEBIAN_MLECOMMON_PACKAGE_DEPENDS
  "percona-server-server-core (>= ${MLE_SERVER_VERSION}), libc6, libstdc++6, libgcc-s1")
IF(MLE_APPARMOR)
  # Where AppArmor is in use, the profile for the launcher and the workers.
  SET(CPACK_DEBIAN_MLECOMMON_PACKAGE_RECOMMENDS "percona-mle-apparmor (= ${mle_deb_ver})")
  SET(CPACK_DEBIAN_MLEAPPARMOR_PACKAGE_NAME percona-mle-apparmor)
  SET(CPACK_DEBIAN_MLEAPPARMOR_PACKAGE_ARCHITECTURE all)
  SET(CPACK_DEBIAN_MLEAPPARMOR_PACKAGE_DEPENDS "percona-mle-common (= ${mle_deb_ver}), apparmor")
  SET(CPACK_DEBIAN_MLEAPPARMOR_DESCRIPTION
    "Percona MLE: AppArmor profile for the MLE launcher and workers")
  # Its files are configuration files; the scripts load the profile.
  SET(CPACK_DEBIAN_MLEAPPARMOR_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle-apparmor/postinst;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle-apparmor/postrm;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle-apparmor/conffiles")
ENDIF()
SET(CPACK_DEBIAN_MLECORE_PACKAGE_NAME percona-mle)
SET(CPACK_DEBIAN_MLECORE_PACKAGE_DEPENDS
  "percona-mle-common (= ${mle_deb_ver})${mle_deb_jdk}, zlib1g")
SET(CPACK_DEBIAN_MLECORE_PACKAGE_CONTROL_EXTRA
  "${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/postinst;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/prerm;${CMAKE_SOURCE_DIR}/packaging/debian/percona-mle/triggers")
SET(CPACK_DEBIAN_MLEJDK_PACKAGE_NAME percona-mle-jdk)
SET(CPACK_DEBIAN_MLEPYTHONRUNTIME_PACKAGE_NAME percona-mle-python-runtime)
SET(CPACK_DEBIAN_MLEPYTHONRUNTIME_PACKAGE_DEPENDS "percona-mle-common (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLEPYTHON_PACKAGE_NAME percona-mle-python)
SET(CPACK_DEBIAN_MLEPYTHON_PACKAGE_DEPENDS
  "percona-mle (= ${mle_deb_ver}), percona-mle-python-runtime (= ${mle_deb_ver})")
SET(CPACK_DEBIAN_MLECPYTHON_PACKAGE_NAME percona-mle-cpython)
# The libpython the worker was built against (the package is per distribution anyway).
SET(CPACK_DEBIAN_MLECPYTHON_PACKAGE_DEPENDS
  "percona-mle-common (= ${mle_deb_ver}), libpython${MLE_PYTHON_MAJOR_MINOR}")
SET(CPACK_DEBIAN_MLECPYTHON_DESCRIPTION
  "Percona MLE: Python stored programs on CPython (mle.worker = CPYTHON): small workers, numpy, no JVM")
SET(CPACK_DEBIAN_MLEV8_PACKAGE_NAME percona-mle-v8)
SET(CPACK_DEBIAN_MLEV8_PACKAGE_DEPENDS "percona-mle-common (= ${mle_deb_ver}), ${mle_libnode_deb}")
SET(CPACK_DEBIAN_MLEV8_DESCRIPTION
  "Percona MLE: JavaScript stored programs on V8 (LANGUAGE JS): small workers, the distribution's libnode")
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_NAME percona-mle-tools)
# mle_package is a Python 3 script; it runs npm, Maven, a GraalPy the DBA brings, or the
# CPython worker's Python (python3-venv): it needs neither build of the workers itself.
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_DEPENDS "percona-mle-common (= ${mle_deb_ver}), python3")
SET(CPACK_DEBIAN_MLETOOLS_PACKAGE_RECOMMENDS "nodejs, npm, maven")

# RPM (checked on Oracle Linux 10)
SET(CPACK_RPM_PACKAGE_RELEASE ${MLE_PACKAGE_RELEASE})
SET(CPACK_RPM_FILE_NAME RPM-DEFAULT)
SET(CPACK_RPM_PACKAGE_LICENSE "GPLv2")
SET(CPACK_RPM_PACKAGE_ARCHITECTURE x86_64)
SET(CPACK_RPM_PACKAGE_AUTOREQPROV OFF)
# The server's directories belong to the server's packages (plugin/debug: its debug package),
# SELinux's to selinux-policy: not to ours.
SET(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  /usr/lib64/mysql /usr/lib64/mysql/plugin /usr/lib64/mysql/plugin/debug
  /usr/lib/mysql /usr/lib/mysql/plugin /usr/lib/mysql/plugin/debug
  /usr/share/selinux /usr/share/selinux/packages /usr/share/selinux/packages/targeted)
# Prebuilt runtime: no stripping, debuginfo or Python byte-compiling of the venv by rpmbuild.
SET(CPACK_RPM_SPEC_MORE_DEFINE "%define __strip /bin/true
%define __os_install_post %{nil}
%define _build_id_links none
%define debug_package %{nil}
%define source_date_epoch_from_changelog 0")
SET(mle_rpm_ver "${MLE_VERSION}-${MLE_PACKAGE_RELEASE}")
SET(CPACK_RPM_MLECOMMON_PACKAGE_NAME percona-mle-common)
SET(CPACK_RPM_MLECOMMON_PACKAGE_REQUIRES "percona-server-server >= ${MLE_SERVER_VERSION}")
IF(MLE_SELINUX)
  # Where SELinux policy is installed, the policy module for the launcher and the workers.
  STRING(APPEND CPACK_RPM_MLECOMMON_PACKAGE_REQUIRES
    ", (percona-mle-selinux = ${mle_rpm_ver} if selinux-policy-targeted)")
ENDIF()
SET(CPACK_RPM_MLECORE_PACKAGE_NAME percona-mle)
SET(CPACK_RPM_MLECORE_PACKAGE_REQUIRES
  "percona-mle-common = ${mle_rpm_ver}${mle_rpm_jdk}")
# The AOT cache: made by file triggers (packaging/rpm/percona-mle.triggers), once per transaction
# that changes the JDK, jars or Python. CPack has no trigger sections: they follow the erase
# scriptlet, the last section before the file list.
FILE(READ ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle.preun mle_preun)
FILE(READ ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle.triggers mle_triggers)
FILE(WRITE ${CMAKE_BINARY_DIR}/packaging/percona-mle.preun "${mle_preun}\n${mle_triggers}")
SET(CPACK_RPM_MLECORE_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_BINARY_DIR}/packaging/percona-mle.preun)
SET(CPACK_RPM_MLEJDK_PACKAGE_NAME percona-mle-jdk)
SET(CPACK_RPM_MLEPYTHONRUNTIME_PACKAGE_NAME percona-mle-python-runtime)
SET(CPACK_RPM_MLEPYTHONRUNTIME_PACKAGE_REQUIRES "percona-mle-common = ${mle_rpm_ver}")
SET(CPACK_RPM_MLEPYTHON_PACKAGE_NAME percona-mle-python)
SET(CPACK_RPM_MLEPYTHON_PACKAGE_REQUIRES
  "percona-mle = ${mle_rpm_ver}, percona-mle-python-runtime = ${mle_rpm_ver}")
SET(CPACK_RPM_MLECPYTHON_PACKAGE_NAME percona-mle-cpython)
# No automatic dependencies (AUTOREQPROV OFF): libpython by its soname.
SET(CPACK_RPM_MLECPYTHON_PACKAGE_REQUIRES
  "percona-mle-common = ${mle_rpm_ver}, ${MLE_PYTHON_SONAME}()(64bit)")
SET(CPACK_RPM_MLECPYTHON_PACKAGE_SUMMARY
  "Percona MLE: Python stored programs on CPython (mle.worker = CPYTHON)")
SET(CPACK_RPM_MLEV8_PACKAGE_NAME percona-mle-v8)
# No automatic dependencies (AUTOREQPROV OFF): libnode by its soname (nodejs-libs).
# nodejs-full-i18n: ICU's data for nodejs-libs' own ICU (Intl; the worker points ICU at it).
SET(CPACK_RPM_MLEV8_PACKAGE_REQUIRES
  "percona-mle-common = ${mle_rpm_ver}, ${MLE_LIBNODE_SONAME}()(64bit), nodejs-full-i18n")
SET(CPACK_RPM_MLEV8_PACKAGE_SUMMARY "Percona MLE: JavaScript stored programs on V8 (LANGUAGE JS)")
SET(CPACK_RPM_MLETOOLS_PACKAGE_NAME percona-mle-tools)
# No automatic dependencies (AUTOREQPROV OFF): the interpreter by name. Weak dependencies as
# in the Debian package (CPack versions that do not know RECOMMENDS leave it out).
SET(CPACK_RPM_MLETOOLS_PACKAGE_REQUIRES "percona-mle-common = ${mle_rpm_ver}, python3")
SET(CPACK_RPM_MLETOOLS_PACKAGE_RECOMMENDS "nodejs, npm, maven")
SET(CPACK_RPM_MLESELINUX_PACKAGE_NAME percona-mle-selinux)
SET(CPACK_RPM_MLESELINUX_PACKAGE_ARCHITECTURE noarch)
SET(CPACK_RPM_MLESELINUX_PACKAGE_REQUIRES "selinux-policy-targeted")
SET(CPACK_RPM_MLESELINUX_PACKAGE_REQUIRES_POST "policycoreutils, selinux-policy-targeted")
SET(CPACK_RPM_MLESELINUX_PACKAGE_REQUIRES_POSTUN "policycoreutils")
SET(CPACK_RPM_MLESELINUX_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle-selinux.post)
SET(CPACK_RPM_MLESELINUX_POST_TRANS_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle-selinux.posttrans)
SET(CPACK_RPM_MLESELINUX_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/rpm/percona-mle-selinux.postun)
SET(CPACK_RPM_MLESELINUX_PACKAGE_SUMMARY "Percona MLE: SELinux policy for the MLE launcher and workers")

INCLUDE(CPack)
