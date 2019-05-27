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

set -euo pipefail

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# More miscellaneous tests

# Locked finalization
booted_csum=$(vm_get_booted_csum)
commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree deploy revision="${commit}" --lock-finalization
vm_cmd test -f /run/ostree/staged-deployment-locked
cursor=$(vm_get_journal_cursor)
vm_reboot
assert_streq "$(vm_get_booted_csum)" "${booted_csum}"
vm_assert_journal_has_content $cursor 'Not finalizing; found /run/ostree/staged-deployment-locked'
echo "ok locked staging"

vm_rpmostree deploy revision="${commit}" --lock-finalization
vm_cmd test -f /run/ostree/staged-deployment-locked
if vm_rpmostree finalize-deployment; then
  assert_not_reached "finalized without expected checksum"
elif vm_rpmostree finalize-deployment WRONG_CHECKSUM; then
  assert_not_reached "finalized with wrong checksum"
fi
cursor=$(vm_get_journal_cursor)
vm_reboot_cmd rpm-ostree finalize-deployment "${commit}"
assert_streq "$(vm_get_booted_csum)" "${commit}"
vm_assert_journal_has_content $cursor "Finalized deployment; rebooting into ${commit}"
echo "ok finalize-deployment"

# Custom origin and local repo rebases. This is essentially the RHCOS workflow.
# https://github.com/projectatomic/rpm-ostree/pull/1406
# https://github.com/projectatomic/rpm-ostree/pull/1732
booted_csum=$(vm_get_booted_csum)
oscontainer_source="oscontainer://quay.io/exampleos@sha256:98ea6e4f216f2fb4b69fff9b3a44842c38686ca685f3f55dc48c5d3fb1107be4"
if vm_rpmostree rebase --skip-purge --custom-origin-url "${oscontainer_source}" \
                :${booted_csum} 2>err.txt; then
    fatal "rebased without description"
fi
assert_file_has_content_literal err.txt '--custom-origin-description must be supplied'
vm_rpmostree rebase --skip-purge --custom-origin-description "'Updated via pivot'" \
             --custom-origin-url "${oscontainer_source}" \
             :${booted_csum}
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'CustomOrigin: Updated via pivot'
assert_file_has_content_literal status.txt "${oscontainer_source}"
vm_rpmostree upgrade >out.txt
assert_file_has_content_literal out.txt 'Pinned to commit by custom origin: Updated via pivot'
vm_rpmostree cleanup -p
echo "ok rebase with custom origin"

# Try again but making it think it's pulling from another local repo
vm_rpmostree rebase --skip-purge /sysroot/ostree/repo:${booted_csum} --experimental
vm_rpmostree upgrade >out.txt
assert_file_has_content_literal out.txt 'Pinned to commit; no upgrade available'
vm_rpmostree cleanup -p
echo "ok rebase from local repo remote"

# Add metadata string containing EnfOfLife attribtue
META_ENDOFLIFE_MESSAGE="this is a test for metadata message"
commit=$(vm_cmd ostree commit -b vmcheck \
            --tree=ref=vmcheck --add-metadata-string=ostree.endoflife="'${META_ENDOFLIFE_MESSAGE}'")
vm_rpmostree upgrade
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""
echo "ok endoflife metadata gets parsed correctly"

# Build a layered commit and check if EndOfLife still present
vm_build_rpm foo
vm_rpmostree install foo
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""
echo "ok layered commit inherits the endoflife attribute"

# Check whether it's staged; `rollback` is only supported with
# the soon-to-be-legacy mode of not staging deployments by default.
if vm_pending_is_staged; then
    vm_assert_status_jq ".deployments[1][\"booted\"] == true"
    if vm_rpmostree rollback 2>err.txt; then
        fatal "rolled back staged?"
    fi
    assert_file_has_content err.txt 'error: Staged.*remove.*cleanup'
    # For the pinning tests, we need two real deployments, so
    # let's reboot now, then we need to get back to the previous state,
    # so reboot again.
    vm_reboot
    vm_rpmostree rollback
    vm_reboot
    vm_rpmostree rollback
else
    vm_rpmostree rollback
    vm_assert_status_jq ".deployments[0][\"booted\"] == true" \
                        ".deployments[1][\"booted\"] == false"
    vm_rpmostree rollback
    vm_assert_status_jq ".deployments[0][\"booted\"] == false" \
                        ".deployments[1][\"booted\"] == true"
    echo "ok rollback"
fi
vm_rpmostree status
echo "before pinning"

# Pinning
vm_cmd ostree admin pin 0 > pin.txt
assert_file_has_content pin.txt 'is now pinned'
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt "Pinned: yes"
vm_cmd ostree admin pin -u 0 > pin.txt
assert_file_has_content_literal pin.txt 'is now unpinned'
vm_rpmostree status > status.txt
assert_not_file_has_content status.txt "Pinned: yes"
echo "ok pinning"

# trying to clean up a pinned pending deployment should be a no-op
vm_cmd ostree admin pin 0 > pin.txt
assert_file_has_content pin.txt 'is now pinned'
vm_assert_status_jq ".deployments|length == 2" \
                    ".deployments[0][\"pinned\"] == true"
vm_rpmostree cleanup -p
vm_assert_status_jq ".deployments|length == 2"
echo "ok pinned pending"

vm_build_rpm bar
vm_rpmostree install bar
vm_assert_status_jq ".deployments|length == 3"
# but that new one shouldn't be pinned
vm_assert_status_jq ".deployments[0][\"pinned\"] == false"
vm_rpmostree cleanup -p
vm_assert_status_jq ".deployments|length == 2"
echo "ok pinning not carried over"

# and now check that we can unpin and cleanup
vm_cmd ostree admin pin -u 0 > pin.txt
assert_file_has_content_literal pin.txt 'is now unpinned'
vm_assert_status_jq ".deployments[0][\"pinned\"] == false"
vm_rpmostree cleanup -p
vm_assert_status_jq ".deployments|length == 1"
echo "ok unpin"

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
vm_rpmostree status
echo "before redeploy"
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
assert_file_has_content err.txt "run.*journalctl.*for more information"
vm_assert_journal_has_content $cursor 'rpm-ostree(bad-post.post).*a bad post'
echo "ok script output prefixed in journal"

vm_build_rpm check-ostree-booted post "test -f /run/ostree-booted"
vm_rpmostree install check-ostree-booted
echo "ok /run/ostree-booted in scriptlet container"

# check refresh-md/-C functionality

# local repos are always cached, so let's start up an http server for the same
# vmcheck repo
vm_start_httpd vmcheck /var/tmp 8888
vm_ansible_inline <<EOF
- copy:
    content: |
      [vmcheck-http]
      name=vmcheck-http
      baseurl=http://localhost:8888/vmcheck/yumrepo
      gpgcheck=0
    dest: /etc/yum.repos.d/vmcheck-http.repo
EOF

vm_rpmostree cleanup -rpmb
vm_cmd rm -f /etc/yum.repos.d/vmcheck.repo
vm_build_rpm_repo_mode skip refresh-md-old-pkg
vm_rpmostree refresh-md | tee out.txt
assert_file_has_content_literal out.txt "Updating metadata for 'vmcheck-http'"
vm_build_rpm_repo_mode skip refresh-md-new-pkg
vm_rpmostree refresh-md | tee out.txt # shouldn't do anything since it hasn't expired yet
assert_file_has_content_literal out.txt "rpm-md repo 'vmcheck-http' (cached)"
if vm_rpmostree install refresh-md-new-pkg --dry-run; then
  assert_not_reached "successfully dry-run installed new pkg from cached rpmmd?"
fi
vm_rpmostree refresh-md -f | tee out.txt
assert_file_has_content_literal out.txt "Updating metadata for 'vmcheck-http'"
if ! vm_rpmostree install refresh-md-new-pkg --dry-run; then
  assert_not_reached "failed to dry-run install new pkg from cached rpmmd?"
fi
vm_stop_httpd vmcheck
echo "ok refresh-md"

# check that a failed staging shows up in status

# first create a staged deployment
vm_build_rpm test-stage-fail
vm_rpmostree install test-stage-fail
vm_pending_is_staged

# OK, now make sure we'll fail. One nuclear way to do this is to just delete the
# deployment root it expects to exist. I played with overriding the service file
# so we just do e.g. /usr/bin/false, but the issue is we still want the "start"
# journal msg to be emitted.
vm_cmd rm -rf $(vm_get_deployment_root 0)

# and now check that we notice there was a failure in `status`
vm_reboot
vm_rpmostree status > status.txt
assert_file_has_content status.txt "failed to finalize previous deployment"
assert_file_has_content status.txt "error: opendir"
echo "ok previous staged failure in status"
