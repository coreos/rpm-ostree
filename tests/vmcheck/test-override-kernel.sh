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

osid=$(vm_cmd grep -E '^ID=' /etc/os-release)
if test "${osid}" != 'ID=fedora'; then
    echo "ok skip on OS ID=${osid}"
    exit 0
fi

versionid=$(vm_cmd grep -E '^VERSION_ID=' /etc/os-release)
versionid=${versionid:11} # trim off VERSION_ID=

# Test that we can override the kernel.  For ease of testing
# I just picked the "gold" F29/30 kernel.
current=$(vm_get_booted_csum)
vm_cmd rpm-ostree db list "${current}" > current-dblist.txt
case $versionid in
  29) kernel_release=4.18.16-300.fc29.x86_64;;
  30) kernel_release=5.0.9-301.fc30.x86_64;;
  *) assert_not_reached "Unsupported Fedora version: $versionid";;
esac
assert_not_file_has_content current-dblist.txt $kernel_release
grep -E '^ kernel-5' current-dblist.txt  | sed -e 's,^ *,,' > orig-kernel.txt
assert_streq "$(wc -l < orig-kernel.txt)" "1"
orig_kernel=$(cat orig-kernel.txt)
URL_ROOT="https://dl.fedoraproject.org/pub/fedora/linux/releases/$versionid/Everything/x86_64/os/Packages/k"
vm_rpmostree override replace \
  "$URL_ROOT/kernel{,-core,-modules{,-extra}}-$kernel_release.rpm"
new=$(vm_get_pending_csum)
vm_cmd rpm-ostree db list "${new}" > new-dblist.txt
assert_file_has_content_literal new-dblist.txt $kernel_release
if grep -q -F -e "${orig_kernel}" new-dblist.txt; then
    fatal "Found kernel: ${line}"
fi
newroot=$(vm_get_deployment_root 0)
vm_cmd find ${newroot}/usr/lib/modules -maxdepth 1 -type d > modules-dirs.txt
assert_streq "$(wc -l < modules-dirs.txt)" "2"
assert_file_has_content_literal modules-dirs.txt $kernel_release

echo "ok override kernel"
