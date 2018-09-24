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

# Check that trying to install multiple nonexistent pkgs at once provides an
# error including all of them at once
fakes="foobar barbaz bazboo"
if vm_rpmostree install $fakes &> err.txt; then
  assert_not_reached "successfully layered non-existent pkgs $fakes?"
fi
assert_file_has_content_literal err.txt "Packages not found:"
# ordering can be different, so check one at a time
for pkg in $fakes; do assert_file_has_content_literal err.txt $pkg; done
echo "ok one error for multiple missing pkgs"

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
# And that we have rpmmd-repos on the layered commit
vm_cmd ostree show --print-metadata-key rpmostree.rpmmd-repos $(vm_get_deployment_info 0 checksum) > rpmmd-meta.txt
assert_file_has_content rpmmd-meta.txt 'id.*test-repo'
assert_file_has_content rpmmd-meta.txt 'timestamp'
rm -f rpmmd-meta.txt

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

# Check that there are no pkglist entries in the --json output
vm_assert_status_jq \
  '.deployments[0]["base-commit-meta"]|index("rpmostree.rpmdb.pkglist")|not' \
  '.deployments[0]["layered-commit-meta"]|index("rpmostree.rpmdb.pkglist")|not'
echo "ok clean --json"

# Test idempotent install
old_pending=$(vm_get_pending_csum)
if vm_rpmostree install foo-1.0 &> out.txt; then
  assert_not_reached "installed foo twice?"
fi
assert_file_has_content_literal out.txt 'already requested'
vm_rpmostree install foo-1.0 --idempotent
assert_streq $old_pending $(vm_get_pending_csum)
echo "ok idempotent install"

vm_rpmostree uninstall foo-1.0

# Test idempotent uninstall
old_pending=$(vm_get_pending_csum)
if vm_rpmostree uninstall foo-1.0 &> out.txt; then
  assert_not_reached "uninstalled foo twice?"
fi
assert_file_has_content_literal out.txt 'not currently requested'
vm_rpmostree uninstall foo-1.0 --idempotent
rc=0
vm_rpmostree uninstall foo-1.0 --idempotent --unchanged-exit-77 || rc=$?
assert_streq $old_pending $(vm_get_pending_csum)
assert_streq $rc 77
echo "ok idempotent uninstall"

# Test `rpm-ostree status --pending-exit-77`
rc=0
vm_rpmostree status --pending-exit-77 || rc=$?
assert_streq $rc 77

# Test that we don't do progress bars if on a tty (with the client)
# (And use --unchanged-exit-77 to verify that we *don't* exit 77).
vm_rpmostree install foo-1.0 --unchanged-exit-77 > foo-install.txt
assert_file_has_content_literal foo-install.txt 'Checking out packages (1/1) 100%'
echo "ok install not on a tty"

# check that by default we diff booted vs pending
vm_rpmostree db diff --format=diff > out.txt
assert_file_has_content out.txt +foo-1.0

vm_reboot

# and check that now by default we diff rollback vs booted
vm_rpmostree db diff --format=diff > out.txt
assert_file_has_content out.txt +foo-1.0

# Test `rpm-ostree status --pending-exit-77`, with no actual pending deployment
rc=0
vm_rpmostree status --pending-exit-77 || rc=$?
assert_streq $rc 0

vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-commit-meta"]' \
  '.deployments[0]["layered-commit-meta"]["rpmostree.clientlayer_version"] > 1'
vm_rpmostree status --verbose > verbose-status.txt
assert_file_has_content_literal '└─ test-repo'

vm_assert_layered_pkg foo-1.0 present
echo "ok pkg foo added"

output=$(vm_cmd /usr/bin/foo)
if [[ $output != foo-1.0-1.x86_64 ]]; then
  assert_not_reached "foo printed wrong output"
fi
echo "ok correct output"

# check that there are no leftover rpmdb files
booted_csum=$(vm_get_booted_csum)
vm_cmd ostree ls $booted_csum /usr/share/rpm > out.txt
assert_not_file_has_content out.txt __db
echo "ok no leftover rpmdb files"

# upgrade to a layer with foo already builtin
vm_ostree_commit_layered_as_base $booted_csum vmcheck
vm_rpmostree upgrade
vm_build_rpm bar conflicts foo
if vm_rpmostree install bar &> err.txt; then
  assert_not_reached "successfully layered conflicting pkg bar?"
fi
assert_file_has_content err.txt "Base packages would be removed"
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
vm_cmd mv /var/tmp/vmcheck/yumrepo{,.bak}
vm_rpmostree pkg-remove foo-1.0
vm_cmd mv /var/tmp/vmcheck/yumrepo{.bak,}
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
