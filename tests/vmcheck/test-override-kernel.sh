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

# Test that we can override the kernel.  For ease of testing
# I just picked the "gold" F27 kernel.
vm_cmd 'curl -sS -L -O https://dl.fedoraproject.org/pub/fedora/linux/releases/27/Everything/x86_64/os/Packages/k/kernel-4.13.9-300.fc27.x86_64.rpm \
                -O https://dl.fedoraproject.org/pub/fedora/linux/releases/27/Everything/x86_64/os/Packages/k/kernel-core-4.13.9-300.fc27.x86_64.rpm \
                -O https://dl.fedoraproject.org/pub/fedora/linux/releases/27/Everything/x86_64/os/Packages/k/kernel-modules-4.13.9-300.fc27.x86_64.rpm \
                -O https://dl.fedoraproject.org/pub/fedora/linux/releases/27/Everything/x86_64/os/Packages/k/kernel-modules-extra-4.13.9-300.fc27.x86_64.rpm'
current=$(vm_get_booted_csum)
vm_cmd rpm-ostree db list "${current}" > current-dblist.txt
assert_not_file_has_content current-dblist.txt 'kernel-4.13.9-300.fc27'
grep -E '^ kernel-' current-dblist.txt  | sed -e 's,^ *,,' > current-kernel.txt
vm_rpmostree override replace ./kernel*4.13.9*.rpm
new=$(vm_get_deployment_info 0 checksum)
vm_cmd rpm-ostree db list "${new}" > new-dblist.txt
assert_file_has_content_literal new-dblist.txt 'kernel-4.13.9-300.fc27'
cat current-kernel.txt | while read line; do
    if grep -q -F -e "${line}" new-dblist.txt; then
        fatal "Found kernel: ${line}"
    fi
done
echo "ok override kernel"
