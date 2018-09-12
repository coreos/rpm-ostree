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

# SUMMARY: Tests for the `initramfs` functionality

base=$(vm_get_booted_csum)

vm_rpmostree initramfs > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*disabled"
echo "ok initramfs status"

if vm_rpmostree initramfs --disable 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at disabling"
fi
assert_file_has_content err.txt "already.*disabled"
if vm_rpmostree initramfs --reboot 2>err.txt; then
    assert_not_reached "reboot worked?"
fi
assert_file_has_content err.txt "reboot.*used with.*enable.*disable"
if vm_rpmostree initramfs --arg=foo 2>err.txt; then
    assert_not_reached "arg worked?"
fi
assert_file_has_content err.txt "arg.*used with.*enable"
echo "ok initramfs state"

vm_rpmostree initramfs --enable > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*enabled"
vm_rpmostree initramfs > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*enabled"

vm_assert_status_jq \
  '.deployments[1].booted' \
  '.deployments[0]["regenerate-initramfs"]' \
  '.deployments[1]["regenerate-initramfs"]|not'

vm_reboot

assert_not_streq $base $(vm_get_booted_csum)
vm_assert_status_jq \
  '.deployments[0].booted' \
  '.deployments[0]["regenerate-initramfs"]' \
  '.deployments[0]["initramfs-args"]|length == 0' \
  '.deployments[1]["regenerate-initramfs"]|not' \
  '.deployments[1]["initramfs-args"]|not'

if vm_rpmostree initramfs --enable 2>err.txt; then
    assert_not_reached "Unexpectedly succeeded at enabling"
fi
assert_file_has_content err.txt "already.*enabled"
echo "ok initramfs enabled"

vm_rpmostree initramfs --disable > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*disabled"
vm_rpmostree initramfs > initramfs.txt
assert_file_has_content initramfs.txt "Initramfs regeneration.*disabled"

vm_reboot
vm_assert_status_jq \
  '.deployments[0].booted' \
  '.deployments[0]["regenerate-initramfs"]|not' \
  '.deployments[1]["regenerate-initramfs"]'

echo "ok initramfs disabled"

vm_reboot_cmd rpm-ostree initramfs --enable --reboot
vm_assert_status_jq \
  '.deployments[0].booted' \
  '.deployments[0]["regenerate-initramfs"]' \
  '.deployments[1]["regenerate-initramfs"]|not'

vm_reboot_cmd rpm-ostree initramfs --disable --reboot
vm_assert_status_jq \
  '.deployments[0].booted' \
  '.deployments[0]["regenerate-initramfs"]|not' \
  '.deployments[1]["regenerate-initramfs"]'

echo "ok initramfs enable disable reboot"

assert_streq $base $(vm_get_booted_csum)
osname=$(vm_get_booted_deployment_info osname)

for file in first second; do
    vm_cmd touch /etc/rpmostree-initramfs-testing-$file
    vm_rpmostree initramfs --enable --arg="-I" --arg="/etc/rpmostree-initramfs-testing-$file"
    vm_rpmostree initramfs > initramfs.txt
    assert_file_has_content initramfs.txt "Initramfs.*args.*-I.*/etc/rpmostree-initramfs-testing-$file"
    vm_reboot
    vm_assert_status_jq \
        '.deployments[0].booted' \
        '.deployments[0]["regenerate-initramfs"]' \
        '.deployments[0]["initramfs-args"]|index("-I") == 0' \
        '.deployments[0]["initramfs-args"]|index("/etc/rpmostree-initramfs-testing-'${file}'") == 1' \
        '.deployments[0]["initramfs-args"]|length == 2'
    initramfs=$(vm_cmd grep ^initrd /boot/loader/entries/ostree-2-$osname.conf | sed -e 's,initrd ,/boot/,')
    test -n "${initramfs}"
    vm_cmd lsinitrd $initramfs > lsinitrd.txt
    assert_file_has_content lsinitrd.txt /etc/rpmostree-initramfs-testing-${file}
done
echo "ok initramfs args enable"

vm_rpmostree initramfs --disable
vm_reboot
initramfs=$(vm_cmd grep ^initrd /boot/loader/entries/ostree-2-$osname.conf | sed -e 's,initrd ,/boot/,')
test -n "${initramfs}"
vm_cmd lsinitrd $initramfs > lsinitrd.txt
assert_not_file_has_content lsinitrd.txt /etc/rpmostree-initramfs-testing

echo "ok initramfs disable"
