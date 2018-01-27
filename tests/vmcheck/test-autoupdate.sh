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

# first, let's make sure the timer is disabled so it doesn't mess up with our
# tests
vm_cmd systemctl disable --now rpm-ostreed-automatic.timer

# Really testing this like a user requires a remote ostree server setup.
# Let's start by setting up the repo.
REMOTE_OSTREE=/ostree/repo/tmp/vmcheck-remote
vm_cmd mkdir -p $REMOTE_OSTREE
vm_cmd ostree init --repo=$REMOTE_OSTREE --mode=archive
vm_start_httpd ostree_server $REMOTE_OSTREE 8888

# We need to build up a history on the server. Rather than wasting time
# composing trees for real, we just use client package layering to create new
# trees that we then "lift" into the server before cleaning them up client-side.

# steal a commit from the system repo and make a branch out of it
lift_commit() {
  checksum=$1; shift
  branch=$1; shift
  vm_cmd ostree pull-local --repo=$REMOTE_OSTREE --disable-fsync \
    /ostree/repo $checksum
  vm_cmd ostree --repo=$REMOTE_OSTREE refs $branch --delete
  vm_cmd ostree --repo=$REMOTE_OSTREE refs $checksum --create=$branch
}

# use a previously stolen commit to create an update on our vmcheck branch,
# complete with version string and pkglist metadata
create_update() {
  branch=$1; shift
  vm_cmd ostree commit --repo=$REMOTE_OSTREE -b vmcheck \
    --tree=ref=$branch --add-metadata-string=version=$branch --fsync=no
  # avoid libtool wrapper here since we're running on the VM and it would try to
  # cd to topsrcdir/use gcc; libs are installed anyway
  vm_cmd /var/roothome/sync/.libs/inject-pkglist $REMOTE_OSTREE vmcheck
}

# (delete ref but don't prune for easier debugging)
vm_cmd ostree refs --repo=$REMOTE_OSTREE vmcheck --delete

# now let's build some pkgs that we'll jury-rig into a base update
# this whole block can be commented out for a speed-up when iterating locally
vm_build_rpm base-pkg-foo version 1.4 release 7
vm_build_rpm base-pkg-bar
vm_build_rpm base-pkg-baz version 1.1 release 1
vm_rpmostree install base-pkg-{foo,bar,baz}
lift_commit $(vm_get_pending_csum) v1
vm_rpmostree cleanup -p
rm -rf $test_tmpdir/yumrepo
vm_build_rpm base-pkg-foo version 1.4 release 8 # upgraded
vm_build_rpm base-pkg-bar version 0.9 release 3 # downgraded
vm_build_rpm base-pkg-boo version 3.7 release 2.11 # added
vm_rpmostree install base-pkg-{foo,bar,boo}
lift_commit $(vm_get_pending_csum) v2
vm_rpmostree cleanup -p

# ok, we're done with prep, now let's rebase on the first revision and install a
# layered package
create_update v1
vm_cmd ostree remote add vmcheckmote --no-gpg-verify http://localhost:8888/
vm_build_rpm layered-cake version 2.1 release 3
vm_rpmostree rebase vmcheckmote:vmcheck --install layered-cake
vm_reboot
vm_rpmostree status -v
vm_assert_status_jq \
    ".deployments[0][\"origin\"] == \"vmcheckmote:vmcheck\"" \
    ".deployments[0][\"version\"] == \"v1\"" \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("layered-cake") >= 0'
echo "ok prep"

# start it up again since we rebooted
vm_start_httpd ostree_server $REMOTE_OSTREE 8888

change_policy() {
  policy=$1; shift
  vm_cmd cp /usr/etc/rpm-ostreed.conf /etc
  cat > tmp.sh << EOF
echo -e "[Daemon]\nAutomaticUpdatePolicy=$policy" > /etc/rpm-ostreed.conf
EOF
  vm_cmdfile tmp.sh
  vm_rpmostree reload
}

# make sure that off means off
change_policy off
vm_rpmostree status | grep 'auto updates disabled'
vm_rpmostree upgrade --trigger-automatic-update-policy > out.txt
assert_file_has_content out.txt "Automatic updates are not enabled; exiting"
echo "ok disabled"

# ok, let's test out check
change_policy check
vm_rpmostree status | grep 'auto updates enabled (check'

# build an *older version* and check that we don't report an update
vm_build_rpm layered-cake version 2.1 release 2
vm_rpmostree upgrade --trigger-automatic-update-policy
vm_rpmostree status -v > out.txt
assert_not_file_has_content out.txt "Available update"

# build a *newer version* and check that we report an update
vm_build_rpm layered-cake version 2.1 release 4
vm_rpmostree upgrade --trigger-automatic-update-policy
vm_rpmostree status > out.txt
assert_file_has_content out.txt "Available update"
assert_file_has_content out.txt "Diff: 1 upgraded"
vm_rpmostree status -v > out.txt
assert_file_has_content out.txt "Upgraded: layered-cake 2.1-3 -> 2.1-4"
# make sure we don't report ostree-based stuff somehow
! grep -A999 'Available update' out.txt | grep "Version"
! grep -A999 'Available update' out.txt | grep "Timestamp"
! grep -A999 'Available update' out.txt | grep "Commit"
echo "ok check mode layered only"

# ok now let's add ostree updates in the picture
create_update v2
vm_rpmostree upgrade --trigger-automatic-update-policy

# make sure we only pulled down the commit metadata
if vm_cmd ostree checkout vmcheckmote:vmcheck --subpath /usr/share/rpm; then
  assert_not_reached "Was able to checkout /usr/share/rpm?"
fi

assert_update() {
  vm_assert_status_jq \
    '.["cached-update"]["origin"] == "vmcheckmote:vmcheck"' \
    '.["cached-update"]["version"] == "v2"' \
    '.["cached-update"]["ref-has-new-commit"] == true' \
    '.["cached-update"]["gpg-enabled"] == false'

  # we could assert more json here, though how it's presented to users is
  # important, and implicitly tests the json
  vm_rpmostree status > out.txt
  assert_file_has_content out.txt 'Diff: 2 upgraded, 1 downgraded, 1 removed, 1 added'

  vm_rpmostree status -v > out.txt
  assert_file_has_content out.txt 'Upgraded: base-pkg-foo 1.4-7 -> 1.4-8'
  assert_file_has_content out.txt "          layered-cake 2.1-3 -> 2.1-4"
  assert_file_has_content out.txt 'Downgraded: base-pkg-bar 1.0-1 -> 0.9-3'
  assert_file_has_content out.txt 'Removed: base-pkg-baz-1.1-1.x86_64'
  assert_file_has_content out.txt 'Added: base-pkg-boo-3.7-2.11.x86_64'
}

assert_update
echo "ok check mode ostree"

assert_default_deployment_is_update() {
  vm_assert_status_jq \
    '.deployments[0]["origin"] == "vmcheckmote:vmcheck"' \
    '.deployments[0]["version"] == "v2"' \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("layered-cake") >= 0'
  vm_rpmostree db list $(vm_get_pending_csum) > list.txt
  assert_file_has_content list.txt 'layered-cake-2.1-4.x86_64'
}

# now let's upgrade and check that it matches what we expect
vm_rpmostree upgrade
assert_default_deployment_is_update
echo "ok upgrade"
