#!/bin/bash
#
# Copyright (C) 2018 Red Hat, Inc.
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

# Test some of the legacy D-Bus APIs that Cockpit still uses. This is also
# useful for making sure we don't break anything when we move those APIs over to
# use the new pkglist data.

# the usual update synthesis trickery
vm_build_rpm pkg1
vm_rpmostree install pkg1
deploy_csum=$(vm_cmd ostree commit -b vmcheck --fsync=no \
                --tree=ref=$(vm_get_pending_csum) \
                --add-metadata-string=version=vDeploy)
# put a pin on it so it doesn't get blown away
vm_cmd ostree refs $deploy_csum --create vmcheck_tmp/pinned
vm_build_rpm pkg2
vm_rpmostree install pkg2
update_csum=$(vm_cmd ostree commit -b vmcheck --fsync=no \
                --tree=ref=$(vm_get_pending_csum) \
                --add-metadata-string=version=vUpdate)
vm_build_rpm pkg3
vm_rpmostree install pkg3
rebase_csum=$(vm_cmd ostree commit -b vmcheck_tmp/other_branch --fsync=no \
                --tree=ref=$(vm_get_pending_csum) \
                --add-metadata-string=version=vRebase)
vm_rpmostree cleanup -p
echo "ok setup"

osname=$(vm_get_booted_deployment_info osname)
ospath=/org/projectatomic/rpmostree1/${osname//-/_}

call_dbus() {
  method=$1; shift
  vm_cmd gdbus call -y -d org.projectatomic.rpmostree1 -o $ospath \
    -m org.projectatomic.rpmostree1.OS.$method "$@"
}

call_dbus GetCachedDeployRpmDiff "vDeploy" [] > out.txt
assert_file_has_content out.txt "<'vmcheck'>"
assert_file_has_content out.txt "$deploy_csum"
assert_file_has_content out.txt "vDeploy"
assert_file_has_content out.txt "pkg1"
assert_not_file_has_content out.txt "pkg2"
assert_not_file_has_content out.txt "pkg3"
echo "ok GetCachedDeployRpmDiff"

# extra quotes for hungry ssh
call_dbus GetCachedUpdateRpmDiff '""' > out.txt
assert_file_has_content out.txt "<'vmcheck'>"
assert_file_has_content out.txt "$update_csum"
assert_file_has_content out.txt "vUpdate"
assert_file_has_content out.txt "pkg1"
assert_file_has_content out.txt "pkg2"
assert_not_file_has_content out.txt "pkg3"
echo "ok GetCachedUpdateRpmDiff"

call_dbus GetCachedRebaseRpmDiff "vmcheck_tmp/other_branch" [] > out.txt
assert_file_has_content out.txt "<'vmcheck_tmp/other_branch'>"
assert_file_has_content out.txt "$rebase_csum"
assert_file_has_content out.txt "vRebase"
assert_file_has_content out.txt "pkg1"
assert_file_has_content out.txt "pkg2"
assert_file_has_content out.txt "pkg3"
echo "ok GetCachedRebaseRpmDiff"
