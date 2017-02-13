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
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not'

# Be sure an unprivileged user exists
vm_cmd getent passwd bin
if vm_cmd "runuser -u bin rpm-ostree pkg-add foo-1.0"; then
    assert_not_reached "Was able to install a package as non-root!"
fi

# Be sure an unprivileged user exists
if vm_rpmostree install test-opt-1.0 2>err.txt; then
    assert_not_reached "Was able to install a package in /opt"
fi
assert_file_has_content err.txt "See https://github.com/projectatomic/rpm-ostree/issues/233"

echo "ok failed to install in opt"

vm_rpmostree pkg-add foo-1.0
vm_cmd ostree --repo=/sysroot/ostree/repo/extensions/rpmostree/pkgcache refs |grep /foo/> refs.txt
pkgref=$(head -1 refs.txt)
vm_cmd ostree --repo=/sysroot/ostree/repo/extensions/rpmostree/pkgcache show --print-metadata-key rpmostree.repo ${pkgref} >refdata.txt
assert_file_has_content refdata.txt 'id.*test-repo'
rm -f refs.txt refdata.txt
echo "ok pkg-add foo"

vm_reboot
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not'

vm_assert_layered_pkg foo-1.0 present
echo "ok pkg foo added"

if ! vm_cmd /usr/bin/foo | grep "Happy foobing!"; then
  assert_not_reached "foo printed wrong output"
fi
echo "ok correct output"

# check that root is a shared mount
# https://bugzilla.redhat.com/show_bug.cgi?id=1318547
if ! vm_cmd "findmnt / -no PROPAGATION" | grep shared; then
    assert_not_reached "root is not mounted shared"
fi

vm_rpmostree pkg-remove foo-1.0
echo "ok pkg-remove foo"

vm_reboot

vm_assert_layered_pkg foo absent
echo "ok pkg foo removed"

vm_rpmostree cleanup -b
vm_assert_status_jq '.deployments|length == 2'
echo "ok baseline cleanup"
vm_rpmostree cleanup -r
vm_assert_status_jq '.deployments|length == 1'
vm_rpmostree cleanup -pr
vm_assert_status_jq '.deployments|length == 1'
vm_rpmostree pkg-add foo-1.0
vm_assert_status_jq '.deployments|length == 2'
vm_rpmostree cleanup -pr
vm_assert_status_jq '.deployments|length == 1'
echo "ok cleanup"
