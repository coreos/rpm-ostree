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

# Verify that the commit is printed in the output
vm_rpmostree status > status.txt
assert_file_has_content status.txt 'Commit:'

# Locked finalization
booted_csum=$(vm_get_booted_csum)
commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree deploy revision="${commit}" --lock-finalization
vm_cmd test -f /run/ostree/staged-deployment-locked
cursor=$(vm_get_journal_cursor)
vm_reboot
assert_streq "$(vm_get_booted_csum)" "${booted_csum}"
vm_assert_journal_has_content $cursor 'Not finalizing; found /run/ostree/staged-deployment-locked'
echo "ok locked deploy staging"
vm_rpmostree rebase :"${commit}" --lock-finalization --skip-purge
vm_cmd test -f /run/ostree/staged-deployment-locked
cursor=$(vm_get_journal_cursor)
vm_reboot
assert_streq "$(vm_get_booted_csum)" "${booted_csum}"
vm_assert_journal_has_content $cursor 'Not finalizing; found /run/ostree/staged-deployment-locked'
echo "ok locked rebase staging"

# This also tests custom client IDs in the journal and interaction with systemd inhibitor locks.
cursor=$(vm_get_journal_cursor)
vm_cmd env RPMOSTREE_CLIENT_ID=testing-agent-id \
       rpm-ostree deploy revision="${commit}" \
       --lock-finalization
vm_cmd test -f /run/ostree/staged-deployment-locked
if vm_rpmostree finalize-deployment; then
  assert_not_reached "finalized without expected checksum"
elif vm_rpmostree finalize-deployment WRONG_CHECKSUM; then
  assert_not_reached "finalized with wrong checksum"
fi
vm_cmd journalctl --after-cursor "'$cursor'" -u rpm-ostreed -o json | jq -r '.AGENT//""' > agent.txt
assert_file_has_content agent.txt testing-agent-id
vm_cmd journalctl --after-cursor "'$cursor'" -u rpm-ostreed -o json | jq -r '.AGENT_SD_UNIT//""' > agent_sd_unit.txt
assert_file_has_content agent_sd_unit.txt session-1.scope
vm_cmd "systemd-inhibit --what=shutdown --mode=block sh -c 'while ! test -f /run/wakeup; do sleep 0.1; done'" &
if vm_rpmostree finalize-deployment "${commit}" 2>err.txt; then
  assert_not_reached "finalized with inhibitor lock in block mode present"
fi
assert_file_has_content err.txt 'Reboot blocked'
vm_cmd test -f /run/ostree/staged-deployment-locked # Make sure that staged deployment is still locked.
vm_cmd touch /run/wakeup
sleep 1 # Wait one second for the process holding the lock to exit.
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
vm_rpmostree rebase --skip-purge /sysroot/ostree/repo:${booted_csum}
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
# Should be "File exists", but work around a bug in older ostree
assert_file_has_content err.txt "\(Operation not permitted\)\|\(File exists\)"
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
vm_rpmostree deploy $(vm_get_booted_csum) --unchanged-exit-77 > out.txt || rc=$?
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
vm_send_inline /etc/yum.repos.d/vmcheck-http.repo <<EOF
[vmcheck-http]
name=vmcheck-http
baseurl=http://localhost:8888/vmcheck/yumrepo
gpgcheck=0
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
vm_cmd_sysroot_rw rm -rf $(vm_get_deployment_root 0)

# and now check that we notice there was a failure in `status`
vm_reboot
vm_rpmostree status > status.txt
assert_file_has_content status.txt "failed to finalize previous deployment"
assert_file_has_content status.txt "error: opendir"
echo "ok previous staged failure in status"

# check that --skip-branch-check indeeds skips branch checking
csum=$(vm_cmd ostree commit -b otherbranch --tree=ref=vmcheck)
if vm_rpmostree deploy $csum 2>err.txt; then
    assert_not_reached "Deployed to commit on different branch"
fi
assert_file_has_content err.txt "Checksum .* not found in .*"
vm_rpmostree cleanup -p
vm_rpmostree deploy $csum --skip-branch-check
vm_rpmostree cleanup -p
echo "ok deploy --skip-branch-check"

# Test `deploy --register-driver` option
# Create and start a transient test-driver.service unit to register our fake driver
cursor=$(vm_get_journal_cursor)
vm_cmd systemd-run --unit=test-driver.service -q -r \
       rpm-ostree deploy \'\' \
       --register-driver=TestDriver
# wait for driver to register
vm_wait_content_after_cursor $cursor 'Txn Deploy on.*successful'
vm_cmd test -f /run/rpm-ostree/update-driver.gv
vm_cmd rpm-ostree status > status.txt
assert_file_has_content status.txt 'AutomaticUpdatesDriver: TestDriver'
vm_cmd rpm-ostree status -v > verbose_status.txt
assert_file_has_content verbose_status.txt 'AutomaticUpdatesDriver: TestDriver (test-driver.service)'
vm_assert_status_jq ".\"update-driver\"[\"driver-name\"] == \"TestDriver\"" \
                    ".\"update-driver\"[\"driver-sd-unit\"] == \"test-driver.service\""
echo "ok deploy --register-driver with empty string revision"

# Ensure that we are prevented from rebasing when an updates driver is registered
commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
if vm_rpmostree rebase :"${commit}" --skip-purge 2>err.txt;then
  assert_not_reached "Rebase with updates driver registered unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
# Bypass updates driver to force a rebase
vm_rpmostree rebase :"${commit}" --skip-purge --bypass-driver 2>err.txt
assert_not_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
vm_rpmostree cleanup -p
echo "ok rebase when updates driver is registered"

# Ensure that we are prevented from deploying when an updates driver is registered
if vm_rpmostree deploy $(vm_get_booted_csum) 2>err.txt;then
  assert_not_reached "Deploy with updates driver registered unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
# Bypass updates driver to force a deploy
vm_rpmostree deploy $(vm_get_booted_csum) --bypass-driver 2>err.txt
assert_not_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
echo "ok deploy when updates driver is registered"

# Ensure that we are prevented from upgrading when an updates driver is registered
if vm_rpmostree upgrade 2>err.txt; then
  assert_not_reached "Upgrade with updates driver registered unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
# Bypass updates driver to force an upgrade
vm_rpmostree upgrade --bypass-driver 2>err.txt
assert_not_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
vm_rpmostree cleanup -p
echo "ok upgrade when updates driver is registered"

# Check that drivers are ignored if inactive even if registered
vm_cmd systemctl stop test-driver.service
vm_rpmostree upgrade --unchanged-exit-77 2>err.txt # notice we do not `--bypass-driver`
assert_not_file_has_content err.txt 'Updates and deployments are driven by TestDriver'
vm_rpmostree cleanup -p
echo "ok ignore inactive registered driver"

# Test that we don't need to --bypass-driver if the systemd unit associated with
# the client's PID is the update driver's systemd unit.
vm_cmd rpm-ostree deploy \'\' \
       --register-driver=OtherTestDriver --bypass-driver
# Make sure OtherTestDriver's systemd unit will be the same as the commandline's
vm_cmd rpm-ostree status -v > verbose_status.txt
assert_file_has_content verbose_status.txt 'AutomaticUpdatesDriver: OtherTestDriver (session-1.scope)'
vm_rpmostree upgrade 2>err.txt
assert_not_file_has_content err.txt 'Updates and deployments are driven by OtherTestDriver'
vm_rpmostree cleanup -p
echo "ok upgrade without --bypass-driver when same systemd unit"
