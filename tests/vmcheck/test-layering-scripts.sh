#!/bin/bash
#
# Copyright (C) 2016 Jonathan Lebon <jlebon@redhat.com>
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

set -e

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# SUMMARY: check that RPM scripts are properly handled during package layering

# do a bunch of tests together so that we only have to reboot once

vm_build_rpm scriptpkg1 \
  pre "groupadd -r scriptpkg1" \
  pretrans "# http://lists.rpm.org/pipermail/rpm-ecosystem/2016-August/000391.html
            echo i should've been ignored && exit 1" \
  posttrans "# Firewalld; https://github.com/projectatomic/rpm-ostree/issues/638
             . /etc/os-release || :
             # See https://github.com/projectatomic/rpm-ostree/pull/647
             for path in /tmp /var/tmp; do
               if test -f \${path}/file-in-host-tmp-not-for-scripts; then
                 echo found file from host /tmp
                 exit 1
               fi
             done"

# check that host /tmp doesn't get mounted
vm_cmd touch /tmp/file-in-host-tmp-not-for-scripts
vm_rpmostree pkg-add scriptpkg1
echo "ok pkg-add scriptpkg1"

vm_reboot

vm_assert_layered_pkg scriptpkg1 present
echo "ok pkg scriptpkg1 added"

vm_cmd "test ! -f /usr/scriptpkg1.posttrans"
echo "ok no embarrassing crud leftover"

# let's check that the group was successfully added
vm_cmd getent group scriptpkg1
echo "ok group scriptpkg1 active"

# And now, things that should fail
vm_build_rpm rofiles-violation \
  post "echo should fail >> /usr/share/licenses/glibc/COPYING"
if vm_rpmostree install rofiles-violation; then
    assert_not_reached "installed test-post-rofiles-violation!"
fi
