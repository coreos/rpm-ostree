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

commondir=$(cd $(dirname $0)/../common && pwd)
. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# SUMMARY: check that package layering respects rpmdb
# METHOD:
#     - test that during a relayer (e.g. upgrade), if a previously layered pkg
#       is now part of the base layer, then we gently drop the pkg as layered
#     - test that layering a pkg that's already in the base layer fails
#     - test that layering a pkg that's already layered fails
#     - test that layering a new pkg that conflicts with a layered pkg fails
#     - test that layering a new pkg that conflicts with a base pkg fails
#     - test that relayering on a base with a conflicting package fails

vm_send_test_repo

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

vm_cmd rpm-ostree pkg-add foo
echo "ok pkg-add foo"

vm_reboot

vm_assert_layered_pkg foo present
echo "ok pkg foo added"

# let's synthesize an upgrade in which the commit we're upgrading to has foo as
# part of its base, so we recommit our current (non-base) layer to the branch
csum=$(vm_cmd ostree commit -b vmcheck --tree=ref=$(vm_get_booted_csum))

# check that upgrading to it will elide the layered pkg from the origin
vm_cmd rpm-ostree upgrade | tee out.txt
assert_file_has_content out.txt "'foo' .* will no longer be layered"
echo "ok layered pkg foo elision msg"

vm_reboot
new_csum=$(vm_get_booted_csum)
if [[ $new_csum != $csum ]]; then
  assert_not_reached "new csum does not refer to expected csum $csum"
fi

if ! vm_has_packages foo; then
  assert_not_reached "pkg foo is not in rpmdb"
elif vm_has_layered_packages foo; then
  assert_not_reached "pkg foo is layered"
fi
echo "ok layered pkg foo elision"

if vm_cmd rpm-ostree pkg-add foo; then
  assert_not_reached "pkg-add foo succeeded even though it's already in rpmdb"
fi
echo "ok can't layer pkg already in base"

if vm_cmd rpm-ostree pkg-add bar; then
  assert_not_reached "pkg-add bar succeeded but it conflicts with foo in base"
fi
echo "ok can't layer conflicting pkg in base"

# let's go back to that first depl in which foo is really layered
vm_cmd rpm-ostree rollback
vm_reboot
vm_assert_layered_pkg foo present

if vm_cmd rpm-ostree pkg-add foo; then
  assert_not_reached "pkg-add foo succeeded even though it's already layered"
fi
echo "ok can't layer pkg already layered"

if vm_cmd rpm-ostree pkg-add bar; then
  assert_not_reached "pkg-add bar succeeded but it conflicts with layered foo"
fi
echo "ok can't layer conflicting pkg already layered"

# let's go back to the original depl without anything
# XXX: this would be simpler if we had an --onto here
vm_cmd rpm-ostree pkg-remove foo
vm_reboot
vm_assert_layered_pkg foo absent
echo "ok pkg-remove foo"

vm_cmd rpm-ostree pkg-add bar
vm_reboot
vm_assert_layered_pkg bar present
echo "ok pkg-add bar"

# now let's try to do an upgrade -- the latest commit there is still the one we
# created at the beginning of this test, containing foo in the base
if vm_cmd rpm-ostree upgrade; then
  assert_not_reached "upgrade succeeded but new base has conflicting pkg foo"
fi
echo "ok can't upgrade with conflicting layered pkg"
