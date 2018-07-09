#!/bin/bash
#
# Copyright (C) 2018 Jonathan Lebon
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

# Prepare an OSTree repo with updates
vm_ostreeupdate_prepare

# ok, we're done with prep, now let's rebase on the first revision and install a
# bunch of layered packages
vm_build_rpm layered-cake version 2.1 release 3
vm_build_rpm layered-enh
vm_build_rpm layered-sec-none
vm_build_rpm layered-sec-low
vm_build_rpm layered-sec-crit
vm_rpmostree rebase vmcheckmote:vmcheck \
  --install layered-cake \
  --install layered-enh \
  --install layered-sec-none \
  --install layered-sec-low \
  --install layered-sec-crit
if vm_cmd 'grep -qE -e "^StageDeployments=true" /etc/rpm-ostreed.conf'; then
    vm_cmd systemctl is-active ostree-finalize-staged.service
fi
vm_reboot
vm_rpmostree status -v
vm_assert_status_jq \
    '.deployments[0]["origin"] == "vmcheckmote:vmcheck"' \
    '.deployments[0]["version"] == "v1"' \
    '.deployments[0]["packages"]|length == 5' \
    '.deployments[0]["packages"]|index("layered-cake") >= 0'
echo "ok prep"

# start it up again since we rebooted
vm_start_httpd ostree_server $REMOTE_OSTREE 8888

vm_rpmostree cleanup -m

# make sure that off means off
vm_change_update_policy off
vm_rpmostree status | grep 'AutomaticUpdates: disabled'
vm_rpmostree upgrade --trigger-automatic-update-policy > out.txt
assert_file_has_content out.txt "Automatic updates are not enabled; exiting"
# check we didn't download any metadata (skip starting dir)
vm_cmd find /var/cache/rpm-ostree | tail -n +2 > out.txt
if [ -s out.txt ]; then
  cat out.txt && assert_not_reached "rpmmd downloaded!"
fi
echo "ok disabled"

# check that --check/--preview still works
vm_rpmostree upgrade --check > out.txt
assert_file_has_content out.txt "No updates available."
vm_rpmostree upgrade --preview > out.txt
assert_file_has_content out.txt "No updates available."
echo "ok --check/--preview no updates"

# ok, let's test out check
vm_change_update_change_policy check
vm_rpmostree status | grep 'AutomaticUpdates: check'

# build an *older version* and check that we don't report an update
vm_build_rpm layered-cake version 2.1 release 2
cursor=$(vm_get_journal_cursor)
vm_cmd systemctl start rpm-ostree-automatic.service
vm_wait_content_after_cursor $cursor 'Txn AutomaticUpdateTrigger.*successful'
vm_rpmostree status -v > out.txt
assert_not_file_has_content out.txt "AvailableUpdate"
# And check the unit name https://github.com/projectatomic/rpm-ostree/pull/1368
vm_cmd journalctl -u rpm-ostreed --after-cursor ${cursor} > journal.txt
assert_file_has_content journal.txt 'client(id:cli.*unit:rpm-ostreed-automatic.service'
rm -f journal.txt

# build a *newer version* and check that we report an update
vm_build_rpm layered-cake version 2.1 release 4
vm_rpmostree upgrade --trigger-automatic-update-policy
vm_rpmostree status > out.txt
assert_file_has_content out.txt "AvailableUpdate"
assert_file_has_content out.txt "Diff: 1 upgraded"
assert_not_file_has_content out.txt "SecAdvisories"
vm_rpmostree status -v > out.txt
assert_file_has_content out.txt "Upgraded: layered-cake 2.1-3 -> 2.1-4"
# make sure we don't report ostree-based stuff somehow
! grep -A999 'AvailableUpdate' out.txt | grep "Version"
! grep -A999 'AvailableUpdate' out.txt | grep "Timestamp"
! grep -A999 'AvailableUpdate' out.txt | grep "Commit"
echo "ok check mode layered only"

# confirm no filelists were fetched
vm_cmd find /var/cache/rpm-ostree -iname '*filelists*' > out.txt
if [ -s out.txt ]; then
  cat out.txt && assert_not_reached "Filelists were downloaded!"
fi
echo "ok no filelists"

# now add some advisory updates
vm_build_rpm layered-enh version 2.0 uinfo VMCHECK-ENH
vm_build_rpm layered-sec-none version 2.0 uinfo VMCHECK-SEC-NONE
vm_rpmostree upgrade --trigger-automatic-update-policy
vm_rpmostree status > out.txt
assert_file_has_content out.txt "SecAdvisories: 1 unknown severity"
vm_rpmostree status -v > out.txt
assert_file_has_content out.txt \
  "SecAdvisories: VMCHECK-SEC-NONE  Unknown    layered-sec-none-2.0-1.x86_64"
assert_not_file_has_content out.txt "VMCHECK-ENH"

assert_output() {
  assert_file_has_content out.txt \
    "SecAdvisories: 1 unknown severity, 1 low, 1 critical"
  assert_file_has_content out-verbose.txt \
    "SecAdvisories: VMCHECK-SEC-NONE  Unknown    layered-sec-none-2.0-1.x86_64" \
    "               VMCHECK-SEC-LOW   Low        layered-sec-low-2.0-1.x86_64" \
    "               VMCHECK-SEC-CRIT  Critical   layered-sec-crit-2.0-1.x86_64"

  # make sure any future call doesn't forget to create fresh ones
  rm -f out.txt out-verbose.txt
}

# now add all of them
vm_build_rpm layered-sec-low version 2.0 uinfo VMCHECK-SEC-LOW
vm_build_rpm layered-sec-crit version 2.0 uinfo VMCHECK-SEC-CRIT
vm_rpmostree upgrade --trigger-automatic-update-policy
vm_rpmostree status > out.txt
vm_rpmostree status -v > out-verbose.txt
assert_output
echo "ok check mode layered only with advisories"

# check we see the same output with --check/--preview
# clear out cache first to make sure they start from scratch
vm_rpmostree cleanup -m
vm_cmd systemctl stop rpm-ostreed
vm_rpmostree upgrade --check > out.txt
vm_rpmostree upgrade --preview > out-verbose.txt
assert_output
echo "ok --check/--preview layered pkgs check policy"

# check that --check/--preview still works even with policy off
vm_change_update_policy off
vm_rpmostree cleanup -m
vm_cmd systemctl stop rpm-ostreed
vm_rpmostree status | grep 'AutomaticUpdates: disabled'
vm_rpmostree upgrade --check > out.txt
vm_rpmostree upgrade --preview > out-verbose.txt
assert_output
echo "ok --check/--preview layered pkgs off policy"

# ok now let's add ostree updates in the picture
vm_change_update_policy check
vm_ostreeupdate_create v2
vm_rpmostree upgrade --trigger-automatic-update-policy

# make sure we only pulled down the commit metadata
if vm_cmd ostree checkout vmcheckmote:vmcheck --subpath /usr/share/rpm; then
  assert_not_reached "Was able to checkout /usr/share/rpm?"
fi

assert_output2() {
  vm_assert_status_jq \
    '.["cached-update"]["origin"] == "vmcheckmote:vmcheck"' \
    '.["cached-update"]["version"] == "v2"' \
    '.["cached-update"]["ref-has-new-commit"] == true' \
    '.["cached-update"]["gpg-enabled"] == false'

  # we could assert more json here, though how it's presented to users is
  # important, and implicitly tests the json
  assert_file_has_content out.txt \
    "SecAdvisories: 1 unknown severity, 1 low, 1 critical" \
    'Diff: 10 upgraded, 1 downgraded, 1 removed, 1 added'

  assert_file_has_content out-verbose.txt \
    "VMCHECK-SEC-NONE  Unknown    base-pkg-sec-none-2.0-1.x86_64" \
    "VMCHECK-SEC-NONE  Unknown    layered-sec-none-2.0-1.x86_64" \
    "VMCHECK-SEC-LOW   Low        base-pkg-sec-low-2.0-1.x86_64" \
    "VMCHECK-SEC-LOW   Low        layered-sec-low-2.0-1.x86_64" \
    "VMCHECK-SEC-CRIT  Critical   base-pkg-sec-crit-2.0-1.x86_64" \
    "VMCHECK-SEC-CRIT  Critical   layered-sec-crit-2.0-1.x86_64" \
    'Upgraded: base-pkg-enh 1.0-1 -> 2.0-1' \
    '          base-pkg-foo 1.4-7 -> 1.4-8' \
    'Downgraded: base-pkg-bar 1.0-1 -> 0.9-3' \
    'Removed: base-pkg-baz-1.1-1.x86_64' \
    'Added: base-pkg-boo-3.7-2.11.x86_64'

  # make sure any future call doesn't forget to create fresh ones
  rm -f out.txt out-verbose.txt
}

vm_rpmostree status > out.txt
vm_rpmostree status -v > out-verbose.txt
assert_output2
echo "ok check mode ostree"

# check that we get similar output with --check/--preview

vm_rpmostree upgrade --check > out.txt
vm_rpmostree upgrade --preview > out-verbose.txt
assert_output2
echo "ok --check/--preview base pkgs check policy"

vm_change_update_policy off
vm_rpmostree cleanup -m
vm_cmd systemctl stop rpm-ostreed
vm_rpmostree upgrade --check > out.txt
vm_rpmostree upgrade --preview > out-verbose.txt
assert_output2
echo "ok --check/--preview base pkgs off policy"

assert_default_deployment_is_update() {
  vm_assert_status_jq \
    '.deployments[0]["origin"] == "vmcheckmote:vmcheck"' \
    '.deployments[0]["version"] == "v2"' \
    '.deployments[0]["packages"]|length == 5' \
    '.deployments[0]["packages"]|index("layered-cake") >= 0'
  vm_rpmostree db list $(vm_get_pending_csum) > list.txt
  assert_file_has_content list.txt 'layered-cake-2.1-4.x86_64'
}

# now let's upgrade and check that it matches what we expect
# (but start from scratch to check that vanilla `upgrade` also builds the cache)
vm_rpmostree cleanup -m
vm_cmd systemctl stop rpm-ostreed
vm_rpmostree upgrade
vm_rpmostree status > out.txt
vm_rpmostree status -v > out-verbose.txt
# we should have printed as part of the pending deployment
assert_not_file_has_content out.txt "Available update"
assert_not_file_has_content out-verbose.txt "Available update"
assert_output2
assert_default_deployment_is_update
echo "ok upgrade"
