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

checksum=$(vm_get_booted_csum)
vm_rpmostree rebase :${checksum}
vm_assert_status_jq ".deployments[0][\"origin\"] == \"${checksum}\""
vm_rpmostree status > status.txt
echo "ok pin to commit"

vm_rpmostree upgrade >out.txt
assert_file_has_content out.txt 'Pinned to commit; no upgrade available'
if vm_rpmostree deploy 42 2>err.txt; then
    fatal "deployed version from commit?"
fi
assert_file_has_content err.txt 'Cannot look up version while pinned to commit'

# And test https://github.com/coreos/rpm-ostree/issues/2603
vm_cmd ostree remote add self --set=gpg-verify=false file:///ostree/repo
vm_rpmostree rebase self:${checksum}
vm_rpmostree upgrade
echo "ok cmds"
