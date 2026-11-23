#!/bin/bash
# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#
# The clients' network of the end-to-end benchmark (perf/e2e/e2e.py calls it; usable by hand):
#
#   net.sh up NAME PREFIX      network namespace NAME; veth NAMEh (PREFIX.1, this host, where mysqld
#                              listens) <-> NAMEc (PREFIX.2, in the namespace); MTU 1500 as an Ethernet LAN
#   net.sh delay NAME MS       netem on NAMEh: the server's packets to the clients wait MS ms (0: none),
#                              so a round trip takes MS ms more
#   net.sh run NAME CMD...     CMD in the namespace as the calling user (environment as given to sudo)
#   net.sh down NAME           removes the namespace and the veth pair
#
# NAME: at most 14 characters. Needs sudo -n (ip, tc, nsenter).
set -e
cmd=$1 name=$2
[ -n "$cmd" ] && [ -n "$name" ] || { sed -n '17,27p' "$0"; exit 2; }
[ ${#name} -le 14 ] || { echo "net.sh: NAME longer than 14 characters" >&2; exit 2; }
case "$cmd" in
up)
  prefix=$3
  sudo -n ip netns add "$name"
  sudo -n ip link add "${name}h" type veth peer name "${name}c"
  sudo -n ip link set "${name}c" netns "$name"
  sudo -n ip addr add "$prefix.1/24" dev "${name}h"
  sudo -n ip link set "${name}h" mtu 1500 up
  sudo -n ip netns exec "$name" ip addr add "$prefix.2/24" dev "${name}c"
  sudo -n ip netns exec "$name" ip link set "${name}c" mtu 1500 up
  sudo -n ip netns exec "$name" ip link set lo up ;;
delay)
  sudo -n tc qdisc del dev "${name}h" root 2>/dev/null || true
  if [ "$3" != 0 ]; then sudo -n tc qdisc add dev "${name}h" root netem delay "$3"ms limit 100000; fi ;;
run)
  shift 2
  exec sudo -n nsenter --net="/run/netns/$name" -S "$(id -u)" -G "$(id -g)" "$@" ;;
down)
  sudo -n ip netns del "$name" 2>/dev/null || true
  sudo -n ip link del "${name}h" 2>/dev/null || true ;;
*)
  echo "net.sh: unknown command $cmd" >&2; exit 2 ;;
esac
