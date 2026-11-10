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

# component_mle: LANGUAGE JAVASCRIPT, PYTHON and JAVA stored programs run in
# MLE worker processes (design: 11-phase4-component.md).
MYSQL_ADD_COMPONENT(mle
  component.cc
  engine.cc
  settings.cc
  status.cc
  LINK_LIBRARIES mle_host
  MODULE_ONLY
  )
# Default of mle.java_home: the GraalVM JDK the worker was built with.
TARGET_COMPILE_DEFINITIONS(component_mle PRIVATE
  MLE_DEFAULT_JAVA_HOME="${MLE_JAVA_HOME}")
# Building the component also builds what it runs (and the build layout).
ADD_DEPENDENCIES(component_mle mle_build_layout)
