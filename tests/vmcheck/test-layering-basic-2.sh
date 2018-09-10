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

# install foo and make sure it was imported
vm_build_rpm foo
vm_rpmostree install foo | tee output.txt
assert_file_has_content output.txt '^Importing (1/1)'

# upgrade with same foo in repos --> shouldn't re-import
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade | tee output.txt
assert_not_file_has_content output.txt '^Importing ('
echo "ok reuse cached pkg"

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

if vm_rpmostree install glibc &>out.txt; then
  assert_not_reached "Successfully requested glibc without --allow-inactive?"
fi
assert_file_has_content out.txt "Use --allow-inactive to disable this check."
vm_rpmostree cleanup -p
vm_rpmostree install glibc --allow-inactive &>out.txt
vm_rpmostree cleanup -p
echo "ok --allow-inactive"

# remove accumulated crud from previous tests
vm_rpmostree uninstall --all
vm_reboot
vm_rpmostree uninstall --all |& tee out.txt
assert_file_has_content out.txt "No change."
vm_build_rpm test-uninstall-all-pkg1
vm_build_rpm test-uninstall-all-pkg2
vm_build_rpm test-uninstall-all-pkg3
# do one from repo and one local for funsies
vm_rpmostree install test-uninstall-all-pkg1 \
  /var/tmp/vmcheck/yumrepo/packages/x86_64/test-uninstall-all-pkg2-1.0-1.x86_64.rpm
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 1'
vm_rpmostree uninstall --all |& tee out.txt
assert_not_file_has_content out.txt "No change."
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 0' \
  '.deployments[0]["requested-packages"]|length == 0' \
  '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree cleanup -p
echo "ok uninstall --all"

vm_rpmostree install test-uninstall-all-pkg1
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["packages"]|index("test-uninstall-all-pkg1") >= 0' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree uninstall --all --install test-uninstall-all-pkg3
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["packages"]|index("test-uninstall-all-pkg3") >= 0' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 0'
echo "ok uninstall --all --install <pkg>"
