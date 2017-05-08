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

# make sure that package-related entries are always present,
# even when they're empty
vm_assert_status_jq \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]'

# Be sure an unprivileged user exists
vm_cmd getent passwd bin
if vm_cmd "runuser -u bin rpm-ostree pkg-add foo-1.0"; then
    assert_not_reached "Was able to install a package as non-root!"
fi

# Assert that we can do status as non-root
vm_cmd "runuser -u bin rpm-ostree status" >/dev/null

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

# install foo and make sure it was imported
vm_rpmostree install foo | tee output.txt
assert_file_has_content output.txt '^Importing:'

# upgrade with same foo in repos --> shouldn't re-import
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade | tee output.txt
assert_not_file_has_content output.txt '^Importing:'

# upgrade with different foo in repos --> should re-import
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
# this is a bit hacky: rpm building is normally handled by make, on which
# vmcheck itself is dependent
c1=$(sha256sum ${commondir}/compose/yum/repo/packages/x86_64/foo-1.0-1.x86_64.rpm)
touch ${commondir}/compose/yum/foo.spec
make -C ${builddir} tests/common/compose/yum/repo/repodata/repomd.xml
c2=$(sha256sum ${commondir}/compose/yum/repo/packages/x86_64/foo-1.0-1.x86_64.rpm)
if cmp -s c1 c2; then
  assert_not_reached "RPM rebuild yielded same SHA256"
fi
vm_send_test_repo
vm_rpmostree upgrade | tee output.txt
assert_file_has_content output.txt '^Importing:'
echo "ok invalidate pkgcache from RPM chksum"
