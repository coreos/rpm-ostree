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
noop_csum=$(vm_cmd ostree commit -b vmcheck --fsync=no \
                --tree=ref=$(vm_get_booted_csum) \
                --add-metadata-string=version=vDeployNoop)
# put a pin on it so it doesn't get blown away
vm_cmd ostree refs $noop_csum --create vmcheck_tmp/pinned
vm_build_rpm pkg1
vm_rpmostree install pkg1
deploy_csum=$(vm_cmd ostree commit -b vmcheck --fsync=no \
                --tree=ref=$(vm_get_pending_csum) \
                --add-metadata-string=version=vDeploy)
# put a pin on it so it doesn't get blown away
vm_cmd ostree refs $deploy_csum --create vmcheck_tmp/pinned2
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

stateroot=$(vm_get_booted_stateroot)
ospath=/org/projectatomic/rpmostree1/${stateroot//-/_}

call_dbus() {
  method=$1; shift
  vm_cmd gdbus call -y -d org.projectatomic.rpmostree1 -o $ospath \
    -m org.projectatomic.rpmostree1.OS.$method "$@"
}

if vm_cmd test -x /usr/bin/python3; then
  py=python3
else
  py=python
fi

run_transaction() {
  method=$1; shift
  sig=$1; shift
  args=$1; shift
  cur=$(vm_get_journal_cursor)
  # use ansible for this so we don't have to think about hungry quote-eating ssh
  vm_shell_inline <<EOF
$py -c '
import dbus
addr = dbus.SystemBus().call_blocking(
  "org.projectatomic.rpmostree1", "$ospath", "org.projectatomic.rpmostree1.OS",
  "$method", "$sig", ($args))
t = dbus.connection.Connection(addr)
t.call_blocking(
  "org.projectatomic.rpmostree1", "/",
  "org.projectatomic.rpmostree1.Transaction", "Start", "", ())
t.close()'
EOF
  vm_wait_content_after_cursor $cur "Txn $method on .* successful"
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

call_dbus GetCachedDeployRpmDiff "vDeployNoop" [] > out.txt
assert_file_has_content out.txt "<'vmcheck'>"
assert_file_has_content out.txt "$noop_csum"
assert_file_has_content out.txt "vDeployNoop"
assert_not_file_has_content out.txt "pkg1"
assert_not_file_has_content out.txt "pkg2"
assert_not_file_has_content out.txt "pkg3"
echo "ok GetCachedDeployRpmDiff no-op"

# This is not a super realistic test since we don't actually download anything.
# Still it checks that we properly update the cache
run_transaction DownloadUpdateRpmDiff "" ""
vm_assert_status_jq '.["cached-update"]["version"] == "vUpdate"'
echo "ok DownloadUpdateRpmDiff"
