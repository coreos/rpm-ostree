#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
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

# make sure that package-related entries are always present,
# even when they're empty
vm_assert_status_jq \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]' \
  '.deployments[0]["requested-local-packages"]' \
  '.deployments[0]["base-removals"]' \
  '.deployments[0]["requested-base-removals"]'
echo "ok empty pkg arrays in status json"

# Be sure an unprivileged user exists and that we can SSH into it. This is a bit
# underhanded, but we need a bona fide user session to verify non-priv status,
# and logging in through SSH is an easy way to achieve that.
if ! vm_cmd getent passwd testuser; then
  vm_cmd useradd testuser
  vm_cmd mkdir -pm 0700 /home/testuser/.ssh
  vm_cmd cp -a /root/.ssh/authorized_keys /home/testuser/.ssh
  vm_cmd chown -R testuser:testuser /home/testuser/.ssh
fi

# Make sure we can't do various operations as non-root
vm_build_rpm foo
if vm_cmd_as testuser rpm-ostree pkg-add foo &> err.txt; then
    assert_not_reached "Was able to install a package as non-root!"
fi
assert_file_has_content err.txt 'PkgChange not allowed for user'
if vm_cmd_as testuser rpm-ostree reload &> err.txt; then
    assert_not_reached "Was able to reload as non-root!"
fi
assert_file_has_content err.txt 'ReloadConfig not allowed for user'
echo "ok auth"

# Assert that we can do status as non-root
vm_cmd_as testuser rpm-ostree status
echo "ok status doesn't require root"

# Also check that we can do status as non-root non-active
vm_cmd runuser -u bin rpm-ostree status
echo "ok status doesn't require active PAM session"

# Reload as root https://github.com/projectatomic/rpm-ostree/issues/976
vm_cmd rpm-ostree reload
echo "ok reload"

# Add metadata string containing EnfOfLife attribtue
META_ENDOFLIFE_MESSAGE="this is a test for metadata message"
commit=$(vm_cmd ostree commit -b vmcheck \
            --tree=ref=vmcheck --add-metadata-string=ostree.endoflife=\"${META_ENDOFLIFE_MESSAGE}\")
vm_rpmostree upgrade
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""
echo "ok endoflife metadata gets parsed correctly"

# Build a layered commit and check if EndOfLife still present
vm_build_rpm foo
vm_rpmostree install foo
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""
echo "ok layered commit inherits the endoflife attribute"

vm_assert_status_jq ".deployments[0][\"booted\"] == false" \
                    ".deployments[1][\"booted\"] == true"
vm_rpmostree rollback
vm_assert_status_jq ".deployments[0][\"booted\"] == true" \
                    ".deployments[1][\"booted\"] == false"
vm_rpmostree rollback
vm_assert_status_jq ".deployments[0][\"booted\"] == false" \
                    ".deployments[1][\"booted\"] == true"
echo "ok rollback"

# https://github.com/ostreedev/ostree/pull/1055
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck --timestamp=\"October 25 1985\"
if vm_rpmostree upgrade 2>err.txt; then
    fatal "upgraded to older commit?"
fi
assert_file_has_content err.txt "chronologically older"
echo "ok failed to upgrade to older commit"

# https://github.com/projectatomic/rpm-ostree/issues/365
vm_build_rpm base-package \
    files /usr/app \
    install "mkdir -p %{buildroot}/usr/app
             echo one > %{buildroot}/usr/app/conflict-file"
vm_rpmostree install base-package

# build a file having exact same content and check for merging
vm_build_rpm test-merging \
    files /usr/app \
    install "mkdir -p %{buildroot}/usr/app
             echo one > %{buildroot}/usr/app/conflict-file"
vm_rpmostree install test-merging
echo "ok identical file merges"

# have a file with same file path but different content, testing for conflicts
vm_build_rpm conflict-pkg \
    files /usr/app \
    install "mkdir -p %{buildroot}/usr/app
             echo two > %{buildroot}/usr/app/conflict-file"
if vm_rpmostree install conflict-pkg 2>err.txt; then
    assert_not_reached "Install packages with conflicting files unexpected succeeded"
fi
assert_not_file_has_content err.txt "Writing rpmdb"
assert_file_has_content err.txt "File exists"
echo "ok detecting file name conflicts before writing rpmdb"

# check that the way we detect deployment changes is not dependent on pending-*
# https://github.com/projectatomic/rpm-ostree/issues/981
vm_rpmostree cleanup -rp
csum=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
# restart to make daemon see the pending checksum
vm_cmd systemctl restart rpm-ostreed
vm_assert_status_jq '.deployments[0]["pending-base-checksum"]'
# hard reset to booted csum (simulates what deploy does to remote refspecs)
vm_cmd ostree reset vmcheck $(vm_get_booted_csum)
rc=0
vm_rpmostree deploy $(vm_get_booted_csum) > out.txt || rc=$?
if [ $rc != 77 ]; then
    assert_not_reached "trying to re-deploy same commit didn't exit 77"
fi
assert_file_has_content out.txt 'No change.'
vm_assert_status_jq '.deployments[0]["pending-base-checksum"]|not'
echo "ok changes to deployment variant don't affect deploy"

vm_build_rpm bad-post post "echo a bad post >&2 && false"
cursor=$(vm_get_journal_cursor)
if vm_rpmostree install bad-post &> err.txt; then
  assert_not_reached "installing pkg with failing post unexpectedly succeeded"
fi
assert_file_has_content err.txt "Run.*journalctl.*for more information"
vm_assert_journal_has_content $cursor 'rpm-ostree(bad-post.post).*a bad post'
echo "ok script output prefixed in journal"
