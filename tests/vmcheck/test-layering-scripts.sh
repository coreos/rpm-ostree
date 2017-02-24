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

vm_send_test_repo

# make sure the package is not already layered
vm_assert_layered_pkg scriptpkg1 absent

# See scriptpkg1.spec
vm_cmd touch /tmp/file-in-host-tmp-not-for-scripts
vm_rpmostree pkg-add scriptpkg1
echo "ok pkg-add scriptpkg1"

vm_reboot

vm_assert_layered_pkg scriptpkg1 present
echo "ok pkg scriptpkg1 added"

# let's check that the group was successfully added
vm_cmd getent group scriptpkg1
echo "ok group scriptpkg1 active"

# And now, things that should fail
if vm_rpmostree install test-post-rofiles-violation; then
    assert_not_reached "installed test-post-rofiles-violation!"
fi
