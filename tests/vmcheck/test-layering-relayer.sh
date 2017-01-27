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

# SUMMARY: check that packages get carried over during reployments from
# upgrades, deploys, and rebases.
# METHOD:
#     Add a package, then test that after an upgrade, deploy, or rebase, we
#     still have the package.

vm_send_test_repo

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

vm_rpmostree pkg-add foo
echo "ok pkg-add foo"

vm_reboot

vm_assert_layered_pkg foo present
echo "ok pkg foo added"

reboot_and_assert_base() {
  vm_reboot
  basecsum=$(vm_get_booted_deployment_info base-checksum)
  if [[ $basecsum != $1 ]]; then
    assert_not_reached "new base-checksum does not refer to expected base $1"
  fi
}

# UPGRADE

assert_status_jq '.deployments[0]["base-checksum"]' \
                 '.deployments[0]["pending-base-checksum"]|not'
# let's synthesize an upgrade
commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree upgrade
assert_status_jq '.deployments[1]["base-checksum"]' \
                 '.deployments[1]["pending-base-checksum"]'
vm_rpmostree status --json > status.json
reboot_and_assert_base $commit
assert_status_jq '.deployments[0]["base-checksum"]' \
                 '.deployments[0]["pending-base-checksum"]|not' \
                 '.deployments[1]["pending-base-checksum"]'
echo "ok upgrade"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on upgrade"

# DEPLOY

commit=$(vm_cmd ostree commit -b vmcheck \
           --tree=ref=vmcheck --add-metadata-string=version=my-commit)
vm_rpmostree deploy my-commit
reboot_and_assert_base $commit
echo "ok deploy"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on deploy"

# REBASE

commit=$(vm_cmd ostree commit -b rebase_test --tree=ref=vmcheck)
vm_rpmostree rebase rebase_test
reboot_and_assert_base $commit
echo "ok rebase"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on rebase"
