#!/bin/bash
#
# Copyright (C) 2019 Jonathan Lebon <jonathan@jlebon.com>
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

# Simple e2e test for history; note there are many more extensive corner-case
# style unit tests in history.rs.

# XXX: hack around the latest ostree in f29 not having
# https://github.com/ostreedev/ostree/pull/1842
vm_rpmostree initramfs --enable

vm_build_rpm foo
vm_rpmostree install foo
vm_reboot
echo "ok setup"

vm_rpmostree ex history > out.txt
assert_file_has_content out.txt "CreateCommand: install foo"
assert_file_has_content out.txt "LayeredPackages: foo"
vm_rpmostree ex history --json | jq . --slurp > out.json
assert_jq out.json \
  '.[0]["deployment-create-command-line"] == "install foo"' \
  '.[0]["deployment-create-timestamp"] != null' \
  '.[0]["deployment"]["packages"][0] == "foo"' \
  '.[0]["first-boot-timestamp"] != null' \
  '.[0]["last-boot-timestamp"] != null' \
  '.[0]["boot-count"] == 1'
echo "ok install first boot"

vm_reboot

vm_rpmostree ex history > out.txt
assert_file_has_content out.txt "BootCount: 2"
vm_rpmostree ex history --json | jq . --slurp > out.json
assert_jq out.json \
  '.[0]["deployment-create-command-line"] == "install foo"' \
  '.[0]["deployment-create-timestamp"] != null' \
  '.[0]["deployment"]["packages"][0] == "foo"' \
  '.[0]["first-boot-timestamp"] != null' \
  '.[0]["last-boot-timestamp"] != null' \
  '.[0]["boot-count"] == 2'
echo "ok install second boot"

vm_rpmostree uninstall foo
vm_reboot

vm_rpmostree ex history > out.txt
assert_file_has_content out.txt "CreateCommand: uninstall foo"
vm_rpmostree ex history --json | jq . --slurp > out.json
assert_jq out.json \
  '.[0]["deployment-create-command-line"] == "uninstall foo"' \
  '.[0]["deployment-create-timestamp"] != null' \
  '.[0]["first-boot-timestamp"] != null' \
  '.[0]["last-boot-timestamp"] != null' \
  '.[0]["boot-count"] == 1'
assert_jq out.json \
  '.[1]["deployment-create-command-line"] == "install foo"' \
  '.[1]["deployment-create-timestamp"] != null' \
  '.[1]["deployment"]["packages"][0] == "foo"' \
  '.[1]["first-boot-timestamp"] != null' \
  '.[1]["last-boot-timestamp"] != null' \
  '.[1]["boot-count"] == 2'
echo "ok uninstall"

# and check history pruning since that's one bit we can't really test from the
# unit tests

vm_cmd find /var/lib/rpm-ostree/history | xargs -n 1 basename | sort -g > entries.txt
if [ ! $(wc -l entries.txt) -gt 1 ]; then
  assert_not_reached "Expected more than 1 entry, got $(cat entries.txt)"
fi

# get the most recent entry
entry=$(tail -n 1 entries.txt)
# And now nuke all the journal entries except the latest.
vm_cmd journalctl --vacuum-time=$((entry - 1))s
vm_rpmostree cleanup -b

vm_cmd ls -l /var/lib/rpm-ostree/history > entries.txt
if [ $(wc -l entries.txt) != 1 ]; then
  assert_not_reached "Expected only 1 entry, got $(cat entries.txt)"
fi
echo "ok prune"
