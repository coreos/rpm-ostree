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

# SUMMARY: basic sanity check of package layering
# METHOD:
#     Add a package, verify that it was added, then remove it, and verify that
#     it was removed.

vm_send_test_repo

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

# Be sure an unprivileged user exists
vm_cmd getent passwd bin
if vm_cmd "runuser -u bin rpm-ostree pkg-add foo-1.0"; then
    assert_not_reached "Was able to install a package as non-root!"
fi

vm_cmd rpm-ostree pkg-add foo-1.0
echo "ok pkg-add foo"

vm_reboot

vm_assert_layered_pkg foo-1.0 present
echo "ok pkg foo added"

if ! vm_cmd /usr/bin/foo | grep "Happy foobing!"; then
  assert_not_reached "foo printed wrong output"
fi
echo "ok correct output"

vm_cmd rpm-ostree pkg-remove foo-1.0
echo "ok pkg-remove foo"

vm_reboot

vm_assert_layered_pkg foo absent
echo "ok pkg foo removed"
