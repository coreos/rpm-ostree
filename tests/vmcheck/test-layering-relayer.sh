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

# SUMMARY: check that packages get carried over during reployments from
# upgrades, deploys, and rebases.
# METHOD:
#     Add a package, then test that after an upgrade, deploy, or rebase, we
#     still have the package.

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

vm_build_rpm foo
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

vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not'
# let's synthesize an upgrade
commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck --bootable)
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[1]["base-checksum"]' \
  '.deployments[1]["pending-base-checksum"]'
vm_rpmostree status --json > status.json
reboot_and_assert_base $commit
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[1]["pending-base-checksum"]'
echo "ok upgrade"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on upgrade"

# DEPLOY

commit=$(vm_cmd ostree commit -b vmcheck \
           --tree=ref=vmcheck --add-metadata-string=version=my-commit --bootable)
vm_rpmostree deploy my-commit
reboot_and_assert_base $commit
echo "ok deploy"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on deploy"

# REBASE

commit=$(vm_cmd ostree commit -b vmcheck_tmp/rebase_test --tree=ref=vmcheck --bootable)
vm_rpmostree rebase --skip-purge vmcheck_tmp/rebase_test
reboot_and_assert_base $commit
echo "ok rebase"

vm_assert_layered_pkg foo present
echo "ok pkg foo relayered on rebase"

# rollup: install/deploy/uninstall

vm_assert_status_jq ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
                 '.deployments[0]["packages"]|index("foo") >= 0' \
                 '.deployments[0]["packages"]|index("bar")|not'
vm_build_rpm bar
vm_rpmostree install bar
vm_assert_status_jq ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
                 '.deployments[0]["packages"]|index("foo") >= 0' \
                 '.deployments[0]["packages"]|index("bar") >= 0'
commit=$(vm_cmd ostree commit -b vmcheck \
                --tree=ref=vmcheck --add-metadata-string=version=my-commit2 --bootable)
vm_rpmostree rebase ${commit}
vm_assert_status_jq ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
                 '.deployments[0]["packages"]|index("foo") >= 0' \
                 '.deployments[0]["packages"]|index("bar") >= 0'
vm_rpmostree uninstall foo
vm_assert_status_jq ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
                 '.deployments[0]["packages"]|index("foo")|not' \
                 '.deployments[0]["packages"]|index("bar") >= 0'
echo "ok rollup"
