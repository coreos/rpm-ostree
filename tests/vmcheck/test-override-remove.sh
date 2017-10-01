#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
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

# create a new vmcheck commit which has foo and bar in it already so that we can
# target them with our override

# make sure the packages are not already layered
vm_assert_layered_pkg foo absent
vm_assert_layered_pkg bar absent
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'

vm_cmd ostree refs $(vm_get_booted_csum) \
  --create vmcheck_tmp/without_foo_and_bar

# create a new branch with foo and bar
vm_build_rpm foo
vm_build_rpm bar
vm_rpmostree install foo bar
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_bar
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_reboot
if ! vm_has_packages foo bar; then
    assert_not_reached "foo or bar not in base layer"
fi
echo "ok setup"

# funky jq syntax: see test-override-local-replace.sh for an explanation of how
# this works. the only difference here is the [.0] which we use to access the
# nevra of each gv_nevra element.

vm_rpmostree ex override remove foo bar
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 2' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 2' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
echo "ok override remove foo and bar"

vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 2' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 2' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
echo "ok override remove carried through upgrade"

vm_rpmostree ex override reset foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
echo "ok override reset foo"

vm_rpmostree ex override reset --all
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'
echo "ok override reset --all"

# check that upgrading to a base without foo works
vm_rpmostree ex override remove foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo_and_bar
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
echo "ok override remove requested but not applied"

# check that upgrading again to a base with foo turns the override back on
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
echo "ok override remove re-applied"
vm_rpmostree cleanup -p

# a few error checks

if vm_rpmostree ex override remove non-existent-package; then
  assert_not_reached "override remove non-existent-package succeeded?"
fi
echo "ok override remove non-existent-package fails"

vm_rpmostree install foo
if vm_rpmostree ex override remove foo; then
  assert_not_reached "override remove layered pkg foo succeeded?"
fi
vm_rpmostree cleanup -p
echo "ok override remove layered pkg foo fails"

# the next two error checks expect an upgraded layer with foo builtin
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar

vm_rpmostree upgrade
vm_rpmostree ex override remove foo
if vm_rpmostree install foo; then
  assert_not_reached "tried to layer pkg removed by override"
fi
# the check blocking this isn't related to overrides, though for consistency
# let's make sure this fails here too
if vm_rpmostree install /tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm; then
  assert_not_reached "tried to layer local pkg removed by override"
fi
vm_rpmostree cleanup -p
echo "ok can't layer pkg removed by override"

vm_build_rpm foo-ext requires "foo = 1.0-1"
vm_rpmostree upgrade --install foo-ext
if vm_rpmostree ex override remove foo; then
  assert_not_reached "override remove base pkg needed by layered pkg succeeded?"
fi
vm_rpmostree cleanup -p
echo "ok override remove base dep to layered pkg fails"
