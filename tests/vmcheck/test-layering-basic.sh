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

# SUMMARY: basic sanity check of package layering
# METHOD:
#     Add a package, verify that it was added, then remove it, and verify that
#     it was removed.

# make sure the package is not already layered
vm_assert_layered_pkg foo absent
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not'

# make sure installing in /opt fails

vm_build_rpm test-opt \
  files /opt/app \
  install "mkdir -p %{buildroot}/opt/app/bin
           touch %{buildroot}/opt/app/bin/foo"
if vm_rpmostree install test-opt-1.0 2>err.txt; then
    assert_not_reached "Was able to install a package in /opt"
fi
assert_file_has_content err.txt "See https://github.com/projectatomic/rpm-ostree/issues/233"

echo "ok failed to install in opt"

vm_build_rpm foo
vm_rpmostree pkg-add foo-1.0
vm_cmd ostree --repo=/sysroot/ostree/repo/extensions/rpmostree/pkgcache refs |grep /foo/> refs.txt
pkgref=$(head -1 refs.txt)
# Verify we have a mapping from pkg-in-ostree → rpmmd-repo info
vm_cmd ostree --repo=/sysroot/ostree/repo/extensions/rpmostree/pkgcache show --print-metadata-key rpmostree.repo ${pkgref} >refdata.txt
assert_file_has_content refdata.txt 'id.*test-repo'
assert_file_has_content refdata.txt 'timestamp'
rm -f refs.txt refdata.txt
# This will cover things like us failing to break hardlinks for the rpmdb,
# as well as rofiles-fuse
vm_cmd ostree fsck
echo "ok pkg-add foo"

vm_reboot
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-commit-meta"]' \
  '.deployments[0]["layered-commit-meta"]["rpmostree.clientlayer_version"] == 2'

vm_assert_layered_pkg foo-1.0 present
echo "ok pkg foo added"

output=$(vm_cmd /usr/bin/foo)
if [[ $output != foo-1.0-1.x86_64 ]]; then
  assert_not_reached "foo printed wrong output"
fi
echo "ok correct output"

# upgrade to a layer with foo already builtin
vm_cmd ostree commit -b vmcheck --tree=ref=$(vm_get_booted_csum)
vm_rpmostree upgrade
vm_build_rpm bar conflicts foo
if vm_rpmostree install bar &> err.txt; then
  assert_not_reached "successfully layered conflicting pkg bar?"
fi
assert_file_has_content err.txt "The following base packages would be removed"
assert_file_has_content err.txt "foo-1.0-1.x86_64"
vm_rpmostree cleanup -p
vm_cmd ostree reset vmcheck $(vm_cmd ostree rev-parse "vmcheck^")
echo "ok can't layer pkg that would remove base pkg"

# check that root is a shared mount
# https://bugzilla.redhat.com/show_bug.cgi?id=1318547
if ! vm_cmd "findmnt / -no PROPAGATION" | grep shared; then
    assert_not_reached "root is not mounted shared"
fi

# test pkg-remove and simultaneously check that it's done without reaching repos
vm_cmd mv /tmp/vmcheck/yumrepo{,.bak}
vm_rpmostree pkg-remove foo-1.0
vm_cmd mv /tmp/vmcheck/yumrepo{.bak,}
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
c1=$(sha256sum ${test_tmpdir}/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm)
vm_build_rpm foo
c2=$(sha256sum ${test_tmpdir}/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm)
if cmp -s c1 c2; then
  assert_not_reached "RPM rebuild yielded same SHA256"
fi
vm_rpmostree upgrade | tee output.txt
assert_file_has_content output.txt '^Importing:'
echo "ok invalidate pkgcache from RPM chksum"

# make sure installing in /boot translates to /usr/lib/ostree-boot
efidir=/boot/EFI/efi/fedora
vm_build_rpm test-boot \
             files "${efidir}/*" \
             install "mkdir -p %{buildroot}/${efidir}/fonts && echo grubenv > %{buildroot}/${efidir}/grubenv && echo unicode > %{buildroot}/${efidir}/fonts/unicode.pf2"
vm_rpmostree install test-boot
vm_reboot
vm_cmd cat /usr/lib/ostree-boot/EFI/efi/fedora/grubenv > grubenv.txt
assert_file_has_content grubenv.txt grubenv
vm_cmd cat /usr/lib/ostree-boot/EFI/efi/fedora/fonts/unicode.pf2 > unicode.txt
assert_file_has_content unicode.txt unicode
echo "ok failed installed in /boot"

