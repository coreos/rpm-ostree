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

set -e

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

vm_send_test_repo

# create a new vmcheck commit which has foo and nonrootcap in it already so that
# we can target them with our override

# make sure the packages are not already layered
vm_assert_layered_pkg foo absent
vm_assert_layered_pkg nonrootcap absent
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not'

vm_cmd ostree refs $(vm_get_booted_csum) --create vmcheck_tmp/without_foo_and_nonrootcap

# create a new branch with foo and nonrootcap already in it
vm_rpmostree install foo nonrootcap
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_nonrootcap
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_nonrootcap
vm_rpmostree upgrade
vm_reboot
if ! vm_has_packages foo nonrootcap; then
    assert_not_reached "foo or nonrootcap not in base layer"
fi
echo "ok setup"

vm_rpmostree ex override remove foo nonrootcap
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 2' \
  '.deployments[0]["removed-base-packages"]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["removed-base-packages"]|index("nonrootcap-1.0-1.x86_64") >= 0'
echo "ok override remove foo and nonrootcap"

vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 2' \
  '.deployments[0]["removed-base-packages"]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["removed-base-packages"]|index("nonrootcap-1.0-1.x86_64") >= 0'
echo "ok override remove carried through upgrade"

vm_rpmostree ex override reset foo
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 1' \
  '.deployments[0]["removed-base-packages"]|index("nonrootcap-1.0-1.x86_64") >= 0'
echo "ok override reset foo"

vm_rpmostree ex override reset --all
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 0'
echo "ok override reset --all"

# check that upgrading to a base without foo drops the override
vm_rpmostree ex override remove foo
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 1' \
  '.deployments[0]["removed-base-packages"]|index("foo-1.0-1.x86_64") >= 0'
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo_and_nonrootcap
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["removed-base-packages"]|length == 0'
echo "ok override drops out"

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
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_nonrootcap

vm_rpmostree upgrade
vm_rpmostree ex override remove foo
if vm_rpmostree install foo; then
  assert_not_reached "tried to layer pkg removed by override"
fi
# the check blocking this isn't related to overrides, though for consistency
# let's make sure this fails here too
if vm_rpmostree install /tmp/vmcheck/repo/packages/x86_64/foo-1.0-1.x86_64.rpm; then
  assert_not_reached "tried to layer local pkg removed by override"
fi
vm_rpmostree cleanup -p
echo "ok can't layer pkg removed by override"

vm_rpmostree upgrade --install foo-ext
if vm_rpmostree ex override remove foo; then
  assert_not_reached "override remove base pkg needed by layered pkg succeeded?"
fi
vm_rpmostree cleanup -p
echo "ok override remove base dep to layered pkg fails"
