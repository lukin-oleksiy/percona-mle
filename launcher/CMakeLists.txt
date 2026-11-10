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

# mle_launcher: starts and isolates MLE worker processes (see main.cc).
# Installation is decided with the rest of the packaging (Phase 3, W7).
MYSQL_ADD_EXECUTABLE(mle_launcher
  main.cc
  child.cc
  cgroup.cc
  LINK_LIBRARIES mle_host
  SKIP_INSTALL
  )
