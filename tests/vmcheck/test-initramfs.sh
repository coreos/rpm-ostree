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

set -e

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# SUMMARY: Tests for the `initramfs` functionality

assert_jq() {
    expression=$1
    jsonfile=$2

    if ! jq -e "${expression}" >/dev/null < $jsonfile; then
        jq . < $jsonfile | sed -e 's/^/# /' >&2
        echo 1>&2 "${expression} failed to match in $jsonfile"
        exit 1
    fi
}

vm_send_test_repo
base=$(vm_get_booted_csum)

vm_cmd rpm-ostree initramfs > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*disabled"
echo "ok initramfs status"

if vm_cmd rpm-ostree initramfs --disable 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at disabling"
fi
assert_file_has_content err.txt "already.*disabled"
echo "ok initramfs state"

vm_cmd rpm-ostree initramfs --enable
vm_cmd rpm-ostree status --json > status.json
assert_jq '.deployments[1].booted' status.json
assert_jq '.deployments[0]["regenerate-initramfs"]' status.json
assert_jq '.deployments[1]["regenerate-initramfs"]|not' status.json

vm_reboot

assert_not_streq $base $(vm_get_booted_csum)
vm_cmd rpm-ostree status --json > status.json
assert_jq '.deployments[0].booted' status.json
assert_jq '.deployments[0]["regenerate-initramfs"]' status.json
assert_jq '.deployments[0]["initramfs-args"]|length == 0' status.json
assert_jq '.deployments[1]["regenerate-initramfs"]|not' status.json
assert_jq '.deployments[1]["initramfs-args"]|not' status.json

if vm_cmd rpm-ostree initramfs --enable 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at enabling"
fi
assert_file_has_content err.txt "already.*enabled"
echo "ok initramfs enabled"

vm_cmd rpm-ostree initramfs --disable
vm_reboot

vm_cmd rpm-ostree status --json > status.json
assert_jq '.deployments[0].booted' status.json
assert_jq '.deployments[0]["regenerate-initramfs"]|not' status.json
assert_jq '.deployments[1]["regenerate-initramfs"]' status.json
assert_streq $base $(vm_get_booted_csum)

echo "ok initramfs disabled"

for file in first second; do
    vm_cmd touch /etc/rpmostree-initramfs-testing-$file
    vm_cmd rpm-ostree initramfs --enable --arg="-I" --arg="/etc/rpmostree-initramfs-testing-$file"
    vm_reboot
    vm_cmd rpm-ostree status --json > status.json
    assert_jq '.deployments[0].booted' status.json
    assert_jq '.deployments[0]["regenerate-initramfs"]' status.json
    assert_jq '.deployments[0]["initramfs-args"]|index("-I") == 0' status.json
    assert_jq '.deployments[0]["initramfs-args"]|index("/etc/rpmostree-initramfs-testing-'${file}'") == 1' status.json
    assert_jq '.deployments[0]["initramfs-args"]|length == 2' status.json
    initramfs=$(vm_cmd grep ^initrd /boot/loader/entries/ostree-fedora-atomic-0.conf | sed -e 's,initrd ,/boot/,')
    test -n "${initramfs}"
    vm_cmd lsinitrd $initramfs > lsinitrd.txt
    assert_file_has_content lsinitrd.txt /etc/rpmostree-initramfs-testing-${file}
done

vm_cmd rpm-ostree initramfs --disable

initramfs=$(vm_cmd grep ^initrd /boot/loader/entries/ostree-fedora-atomic-0.conf | sed -e 's,initrd ,/boot/,')
test -n "${initramfs}"
vm_cmd lsinitrd $initramfs > lsinitrd.txt
assert_not_file_has_content lsinitrd.txt /etc/rpmostree-initramfs-testing

echo "ok initramfs args"
