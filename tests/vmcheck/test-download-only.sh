#!/bin/bash
#
# Copyright (C) 2017 Red Hat Inc.
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

# SUMMARY: basic sanity check of --download-only and --cache-only

# To truly test --download-only/--cache-only, we need to set up an ostree remote
# and a remote yum repo. We could use the same ostree repo here since
# my-remote:my-ref != my-ref, but let's be extra realistic by having two
# completely separate repos.

# seed with a first package so we have a valid repo there
vm_build_rpm_repo_mode skip foobar
vm_start_httpd vmcheck /tmp 8888
vm_rpmostree cleanup -m
vm_ansible_inline <<EOF
- copy:
    content: |
      [vmcheck-http]
      name=vmcheck-http
      baseurl=http://localhost:8888/vmcheck/yumrepo
      gpgcheck=0
    dest: /etc/yum.repos.d/vmcheck-http.repo
EOF

osname=$(vm_get_booted_deployment_info osname)
# use the var through /sysroot/ to make sure we always get hardlinks
remote_repo=/ostree/deploy/$osname/var/tmp/vmcheck/repo
REMOTE_OSTREE="vm_cmd ostree --repo=$remote_repo"
vm_cmd rm -rf $remote_repo
vm_cmd mkdir -p $remote_repo
$REMOTE_OSTREE init --mode=bare
$REMOTE_OSTREE pull-local /ostree/repo vmcheck
vm_cmd ostree remote delete --if-exists vmcheck_remote
vm_cmd ostree remote add --no-gpg-verify vmcheck_remote file://$remote_repo

go_offline() {
  vm_cmd mv ${remote_repo}{,.bak}
  vm_cmd mv /var/tmp/vmcheck/yumrepo{,.bak}
  YUMREPO=/var/tmp/vmcheck/yumrepo.bak/packages/x86_64
}

go_online() {
  vm_cmd mv /var/tmp/vmcheck/yumrepo{.bak,}
  vm_cmd mv ${remote_repo}{.bak,}
  YUMREPO=/var/tmp/vmcheck/yumrepo/packages/x86_64
}

# sanity check
go_offline
if vm_rpmostree makecache; then
  assert_not_reached "able to reach moved yum repo?"
fi
if vm_rpmostree rebase --remote vmcheck_remote; then
  assert_not_reached "able to reach moved ostree repo?"
fi
go_online
vm_rpmostree makecache
vm_rpmostree rebase --remote vmcheck_remote
vm_rpmostree cleanup -pm
echo "ok setup"

csum=$($REMOTE_OSTREE commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree rebase --remote vmcheck_remote --install foobar --download-only
vm_assert_status_jq ".deployments|length == 1" \
                    ".deployments[0][\"booted\"] == true"
go_offline
vm_rpmostree rebase --remote vmcheck_remote --install foobar --cache-only
vm_assert_status_jq ".deployments|length == 2" \
                    ".deployments[0][\"booted\"] == false" \
                    ".deployments[1][\"booted\"] == true" \
                    ".deployments[0][\"base-checksum\"] == \"$csum\"" \
                    ".deployments[0][\"packages\"]|length == 1" \
                    ".deployments[0][\"packages\"]|index(\"foobar\") >= 0"
go_online
echo "ok offline rebase & install"

rc=0
vm_rpmostree upgrade --upgrade-unchanged-exit-77 || rc=$?
assert_streq "$rc" "77"
vm_rpmostree upgrade -C --upgrade-unchanged-exit-77 || rc=$?
assert_streq "$rc" "77"
echo "ok check for change with --cache-only"

$REMOTE_OSTREE commit -b vmcheck --tree=ref=vmcheck --timestamp '"Oct 21 1988"'
vm_cmd ostree pull vmcheck_remote:vmcheck
if vm_rpmostree upgrade -C |& tee out.txt; then
  assert_not_reached "upgraded to chronologically older commit"
fi
assert_file_has_content out.txt 'chronologically older'
vm_rpmostree upgrade -C --allow-downgrade
echo "ok --cache-only still checks commit timestamp"

if vm_rpmostree upgrade --cache-only --download-only; then
  assert_not_reached "allowed --cache-only and --download-only?"
fi
echo "ok conflicting options"

vm_rpmostree cleanup -prmb
vm_assert_status_jq ".deployments|length == 1" \
                    ".deployments[0][\"booted\"] == true"
vm_build_rpm_repo_mode skip barbaz
vm_rpmostree install --download-only foobar $YUMREPO/barbaz-1.0-1.x86_64.rpm
vm_assert_status_jq ".deployments|length == 1" \
                    ".deployments[0][\"booted\"] == true"
go_offline
vm_rpmostree install --cache-only foobar $YUMREPO/barbaz-1.0-1.x86_64.rpm
go_online
vm_assert_status_jq ".deployments|length == 2" \
                    ".deployments[0][\"booted\"] == false" \
                    ".deployments[1][\"booted\"] == true" \
                    ".deployments[0][\"requested-packages\"]|length == 1" \
                    ".deployments[0][\"requested-local-packages\"]|length == 1"
echo "ok offline local RPM install"

# synthesize update with foobar and barbaz builtin
pending=$(vm_get_deployment_info 0 checksum)
$REMOTE_OSTREE pull-local /ostree/repo $pending
$REMOTE_OSTREE commit -b vmcheck --tree=ref=$pending
vm_rpmostree cleanup -prmb
vm_rpmostree rebase --remote vmcheck_remote
vm_build_rpm_repo_mode skip foobar version 2.0
vm_rpmostree override replace $YUMREPO/foobar-2.0-1.x86_64.rpm
csum=$($REMOTE_OSTREE commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree upgrade --download-only
vm_assert_status_jq \
  ".deployments[0][\"base-checksum\"] != \"$csum\"" \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1'
go_offline
vm_rpmostree upgrade --cache-only
vm_assert_status_jq \
  ".deployments[0][\"base-checksum\"] == \"$csum\"" \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1'
go_online
echo "ok offline upgrade with local RPM replacement"

vm_stop_httpd vmcheck
