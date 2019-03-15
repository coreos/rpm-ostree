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

osid=$(vm_cmd grep -E '^ID=' /etc/os-release)
if test "${osid}" != 'ID=fedora'; then
    echo "ok skip on OS ID=${osid}"
    exit 0
fi

# Test rebasing to https://pagure.io/fedora-atomic-host-continuous
# in rojig:// mode.

vm_cmd 'cat >/etc/yum.repos.d/fahc.repo' <<EOF
[fahc]
baseurl=https://ci.centos.org/artifacts/sig-atomic/fahc/jigdo
gpgcheck=0
EOF

if vm_rpmostree rebase rojig://fahc:fedora-atomic-host 2>err.txt; then
    fatal "Did rojig rebase without --experimental"
fi
assert_file_has_content_literal err.txt 'rojig:// refspec requires --experimental'
vm_rpmostree rebase --experimental rojig://fahc:fedora-atomic-host
vm_assert_status_jq '.deployments[0].origin|startswith("rojig://fahc:fedora-atomic-host")'
vm_cmd ostree refs > refs.txt
assert_file_has_content refs.txt '^rpmostree/rojig/kernel-core/'
echo "ok rojig client rebase "

version=$(vm_get_deployment_info 0 version)
version_major=$(echo ${version} | cut -f 1 -d '.')
version_minor=$(echo ${version} | cut -f 2 -d '.')
prev_version=${version_major}.$((${version_minor} - 1))
assert_not_streq "${version}" "${prev_version}"
vm_rpmostree deploy ${prev_version}
vm_assert_status_jq '.deployments[0].origin|startswith("rojig://fahc:fedora-atomic-host")' \
                    '.deployments[0].version == "'${prev_version}'"'

echo "ok rojig client deploy"

vm_cmd rpm-ostree status > status.txt
assert_file_has_content_literal status.txt  'fahc:fedora-atomic-host-'${prev_version}'-1.fc29.x86_64'

echo "ok rojig status"
