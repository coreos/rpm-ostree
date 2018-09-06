#!/bin/bash
#
# Copyright (C) 2017 Red Hat Inc.
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

vm_assert_layered_pkg foo absent

vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) --create vmcheck_tmp/without_foo
vm_build_rpm foo version 1.2 release 3
vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.2-3.x86_64.rpm
echo "ok install foo locally"

vm_reboot

vm_assert_status_jq '.deployments[0]["packages"]|length == 0'
vm_assert_status_jq '.deployments[0]["requested-packages"]|length == 0'
vm_assert_status_jq '.deployments[0]["requested-local-packages"]|length == 1'
vm_has_local_packages foo-1.2-3.x86_64
vm_assert_layered_pkg foo-1.2-3.x86_64 present
echo "ok pkg foo added locally"

# check we could uninstall the package using either its NEVRA or name
vm_rpmostree uninstall foo-1.2-3.x86_64
vm_assert_status_jq '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree cleanup -p
vm_rpmostree uninstall foo
vm_assert_status_jq '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree cleanup -p
echo "ok uninstall by NEVRA or name"

# check that we can still request foo and it's dormant
vm_rpmostree install foo

vm_assert_status_jq '.deployments[0]["packages"]|length == 0'
vm_assert_status_jq '.deployments[0]["requested-packages"]|length == 1'
vm_assert_status_jq '.deployments[0]["requested-local-packages"]|length == 1'
echo "ok request foo"

# check that uninstalling the local rpm makes us go back to repos
vm_rpmostree uninstall foo-1.2-3.x86_64

vm_assert_status_jq '.deployments[0]["packages"]|length == 1'
vm_assert_status_jq '.deployments[0]["requested-packages"]|length == 1'
vm_assert_status_jq '.deployments[0]["requested-local-packages"]|length == 0'
echo "ok layer foo back from repos"

# check that trying to install a package already in the base errors out
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) --create vmcheck_tmp/with_foo
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo
vm_rpmostree uninstall foo
vm_rpmostree upgrade # upgrades to new base which has foo
if vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.2-3.x86_64.rpm; then
  assert_not_reached "didn't error out when trying to install same pkg"
fi
echo "ok error on layering same pkg in base"

# check that installing local RPMs without any repos available works
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo
vm_rpmostree upgrade
vm_cmd rm -rf /etc/yum.repos.d/
vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.2-3.x86_64.rpm
echo "ok layer local foo without repos"
