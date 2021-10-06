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

set -euo pipefail

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# SUMMARY: check that package layering respects rpmdb
# METHOD:
#     - test that during a relayer (e.g. upgrade), if a previously layered pkg
#       is now part of the base layer, then we stop layering but still keep it
#       in the origin
#     - test that layering a pkg that's already in the base layer works
#     - test that layering a pkg that's already layered fails (string match)
#     - test that layering a different provides that's already layered works
#     - test that layering a new pkg that conflicts with a layered pkg fails
#     - test that layering a new pkg that conflicts with a base pkg fails
#     - test that relayering on a base with a conflicting package fails

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

# remember this current commit for later
vm_cmd ostree refs $(vm_get_booted_csum) --create vmcheck_tmp/without_foo

vm_build_rpm foo
vm_rpmostree install foo
echo "ok install foo"

vm_reboot

vm_assert_layered_pkg foo present
echo "ok pkg foo added"

# let's synthesize an upgrade in which the commit we're upgrading to has foo as
# part of its base, so we recommit our current (non-base) layer to the branch

# remember it for later
vm_cmd ostree refs $(vm_get_booted_csum) --create vmcheck_tmp/with_foo
csum_with_foo=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo)

# check that upgrading to it will make the package dormant

vm_rpmostree upgrade
vm_reboot
if [[ $(vm_get_booted_csum) != $csum_with_foo ]]; then
  assert_not_reached "new csum does not refer to expected csum $csum_with_foo"
fi

if ! vm_has_dormant_packages foo; then
  assert_not_reached "pkg foo is not dormant"
fi

echo "ok layered to dormant"

vm_build_rpm bar conflicts foo
if vm_rpmostree pkg-add bar; then
  assert_not_reached "pkg-add bar succeeded but it conflicts with foo in base"
fi
echo "ok can't layer conflicting pkg (dormant)"

# now check that upgrading to a new base layer that drops foo relayers it

vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo
vm_rpmostree upgrade
vm_reboot

vm_assert_layered_pkg foo present
echo "ok dormant to layered"

if vm_rpmostree pkg-add foo; then
  assert_not_reached "pkg-add foo succeeded even though it's already layered"
fi
echo "ok can't layer pkg already layered"

if vm_rpmostree pkg-add bar; then
  assert_not_reached "pkg-add bar succeeded but it conflicts with layered foo"
fi
echo "ok can't layer conflicting pkg (layered)"

# ok, now let's go back to the depl where foo is in the layer
vm_rpmostree rollback
vm_rpmostree pkg-remove foo
vm_reboot

if vm_has_requested_packages foo; then
  assert_not_reached "foo is still in the origin"
fi
echo "ok pkg-remove foo"

if vm_rpmostree pkg-add bar; then
  assert_not_reached "pkg-add bar succeeded but it conflicts with foo in base"
fi
echo "ok can't layer conflicting pkg (base)"

# ok, now go back to a base layer without foo and add bar
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo
vm_rpmostree upgrade
vm_rpmostree pkg-add bar
vm_reboot

vm_assert_layered_pkg bar present
echo "ok pkg-add bar"

# now let's try to do an upgrade to a base layer which *has* foo
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo
if vm_rpmostree upgrade; then
  assert_not_reached "upgrade succeeded but new base has conflicting pkg foo"
fi
echo "ok can't upgrade with conflicting layered pkg"

vm_rpmostree cleanup -p

check_bloop_color() {
  color=$1; shift
  root=$(vm_get_deployment_root 0)
  vm_cmd file $root/usr/bin/bloop > file.txt
  assert_file_has_content file.txt $color
}

# now let's build two pkgs with different colors
# we use -nostdlib so we don't pull in glibc.i686
vm_build_rpm bloop arch i686 build 'echo "void main() {}" | gcc -m32 -nostdlib -o bloop -xc -'
vm_build_rpm bloop arch x86_64 build 'echo "void main() {}" | gcc -m64 -nostdlib -o bloop -xc -'

# first try to install both of them
vm_rpmostree install bloop.i686 bloop.x86_64
check_bloop_color 64-bit
vm_rpmostree cleanup -p
echo "ok coloring layered"

# now embed the x86_64 version into it
vm_rpmostree install bloop.x86_64
vm_cmd ostree commit -b vmcheck --tree=ref=$(vm_get_pending_csum) --fsync=no
vm_rpmostree cleanup -p
vm_rpmostree upgrade
check_bloop_color 64-bit
# now let's pkglayer the i686 version
vm_rpmostree install bloop.i686
# check that the x86_64 version won
check_bloop_color 64-bit
echo "ok coloring skip"

# now embed the 32-bit version instead
vm_rpmostree cleanup -p
vm_rpmostree install bloop.i686
vm_cmd ostree commit -b vmcheck --tree=ref=$(vm_get_pending_csum) --fsync=no
vm_rpmostree cleanup -p
vm_rpmostree upgrade
check_bloop_color 32-bit
# now let's pkglayer the x86_64 version
vm_rpmostree install bloop.x86_64
# and check that the x86_64 version won
check_bloop_color 64-bit
echo "ok coloring replace"
