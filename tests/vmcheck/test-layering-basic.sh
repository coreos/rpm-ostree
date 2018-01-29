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

# make sure installing in /opt and /usr/local fails

vm_build_rpm test-opt \
  files /opt/app \
  install "mkdir -p %{buildroot}/opt/app/bin
           touch %{buildroot}/opt/app/bin/foo"
if vm_rpmostree install test-opt-1.0 2>err.txt; then
    assert_not_reached "Was able to install a package in /opt"
fi
assert_file_has_content err.txt "See https://github.com/projectatomic/rpm-ostree/issues/233"

# https://developer.download.nvidia.com/compute/cuda/repos/rhel7/x86_64/cuda-license-9-0-9.0.176-1.x86_64.rpm
# was known to do this.
vm_build_rpm test-usrlocal \
             files /usr/local/bin/foo \
             install "mkdir -p %{buildroot}/usr/local/bin/
                      touch %{buildroot}/usr/local/bin/foo"
if vm_rpmostree install test-usrlocal-1.0 2>err.txt; then
    assert_not_reached "Was able to install a package in /usr/local/"
fi
assert_file_has_content err.txt "See https://github.com/projectatomic/rpm-ostree/issues/233"

echo "ok failed to install in /opt and /usr/local"

# Explicit epoch of 0 as it's a corner case:
# https://github.com/projectatomic/rpm-ostree/issues/349
vm_build_rpm foo epoch 0
vm_rpmostree pkg-add foo-1.0
vm_cmd ostree refs |grep /foo/> refs.txt
pkgref=$(head -1 refs.txt)
# Verify we have a mapping from pkg-in-ostree → rpmmd-repo info
vm_cmd ostree show --print-metadata-key rpmostree.repo ${pkgref} >refdata.txt
assert_file_has_content refdata.txt 'id.*test-repo'
assert_file_has_content refdata.txt 'timestamp'
rm -f refs.txt refdata.txt
# This will cover things like us failing to break hardlinks for the rpmdb,
# as well as rofiles-fuse
vm_cmd ostree fsck
vm_cmd ostree show --print-metadata-key rpmostree.rpmdb.pkglist \
       $(vm_get_deployment_info 0 checksum) > variant-pkglist.txt
# 0 shows up in variant dump
assert_file_has_content_literal 'variant-pkglist.txt' "('foo', '0', '1.0', '1', 'x86_64')"
# But no 0: in e.g. db diff output, which uses pkglist metadata
vm_rpmostree db diff --format=diff \
  $(vm_get_deployment_info 0 base-checksum) \
  $(vm_get_deployment_info 0 checksum) > db-diff.txt
assert_file_has_content_literal 'db-diff.txt' "+foo-1.0-1.x86_64"
echo "ok pkg-add foo"

# Test that we don't do progress bars if on a tty (with the client)
vm_rpmostree uninstall foo-1.0
vm_rpmostree install foo-1.0 > foo-install.txt
assert_file_has_content_literal foo-install.txt 'Building filesystem (1/1) 100%'
echo "ok install not on a tty"

vm_reboot
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-commit-meta"]' \
  '.deployments[0]["layered-commit-meta"]["rpmostree.clientlayer_version"] > 1'

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
assert_file_has_content output.txt '^Importing (1/1)'

# upgrade with same foo in repos --> shouldn't re-import
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade | tee output.txt
assert_not_file_has_content output.txt '^Importing ('

# upgrade with different foo in repos --> should re-import
c1=$(sha256sum ${test_tmpdir}/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm)
vm_build_rpm foo
c2=$(sha256sum ${test_tmpdir}/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm)
if cmp -s c1 c2; then
  assert_not_reached "RPM rebuild yielded same SHA256"
fi
vm_rpmostree upgrade | tee output.txt
assert_file_has_content output.txt '^Importing (1/1)'
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

# there should be a couple of pkgs already installed from the tests up above,
# though let's add our own to be self-contained
vm_build_rpm test-pkgcache-migrate-pkg1
vm_build_rpm test-pkgcache-migrate-pkg2
vm_rpmostree install test-pkgcache-migrate-pkg{1,2}

# jury-rig a pkgcache of the olden days
OLD_PKGCACHE_DIR=/ostree/repo/extensions/rpmostree/pkgcache
vm_cmd test -L ${OLD_PKGCACHE_DIR}
vm_cmd rm ${OLD_PKGCACHE_DIR}
vm_cmd mkdir ${OLD_PKGCACHE_DIR}
vm_cmd ostree init --repo ${OLD_PKGCACHE_DIR} --mode=bare
vm_cmd ostree pull-local --repo ${OLD_PKGCACHE_DIR} /ostree/repo \
  rpmostree/pkg/test-pkgcache-migrate-pkg{1,2}/1.0-1.x86__64
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
cursor=$(vm_get_journal_cursor)
vm_rpmostree upgrade | tee output.txt
vm_wait_content_after_cursor $cursor 'migrated 2 cached packages'
assert_file_has_content output.txt "Migrating pkgcache"
for ref in rpmostree/pkg/test-pkgcache-migrate-pkg{1,2}/1.0-1.x86__64; do
  vm_cmd ostree show $ref
done
vm_cmd test -L ${OLD_PKGCACHE_DIR}
echo "ok migrate pkgcache"

vm_cmd ostree show --print-metadata-key rpmostree.rpmdb.pkglist \
  $(vm_get_deployment_info 0 checksum) > pkglist.txt
assert_file_has_content pkglist.txt 'test-pkgcache-migrate-pkg'
echo "ok layered pkglist"
