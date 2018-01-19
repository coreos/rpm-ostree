# Shared functions for compose/ex-container tests
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

setup_rpmmd_repos() {
    dest=$1
    shift
    repos=${RPMOSTREE_COMPOSE_TEST_USE_REPOS:-/etc/yum.repos.d}
    for x in ${repos}/fedora{,-updates}.repo; do
        bn=$(basename ${x})
        cp $x ${dest}/${bn}
    done
}

