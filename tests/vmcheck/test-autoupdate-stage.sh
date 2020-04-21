#!/bin/bash
#
# Copyright (C) 2018 Jonathan Lebon
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

# Prepare an OSTree repo with updates
vm_ostreeupdate_prepare_reboot

vm_ostreeupdate_create v2

vm_rpmostree cleanup -m

vm_rpmostree status --verbose > status.txt
assert_file_has_content status.txt 'AutomaticUpdates: disabled'
vm_change_update_policy stage
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'AutomaticUpdates: stage; rpm-ostreed-automatic.timer: inactive'
# And test that we still support "ex-stage"
vm_change_update_policy ex-stage
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'AutomaticUpdates: stage; rpm-ostreed-automatic.timer: inactive'

vm_rpmostree upgrade --trigger-automatic-update-policy
vm_assert_status_jq ".deployments[1][\"booted\"]" \
                    ".deployments[0][\"staged\"]" \
                    ".deployments[0][\"version\"] == \"v2\""
vm_rpmostree status -v > status.txt
assert_file_has_content status.txt "Staged: yes"
vm_rpmostree upgrade > upgrade.txt
assert_file_has_content_literal upgrade.txt 'note: automatic updates (stage) are enabled'
# And ensure that we have new content in /etc after staging
vm_cmd echo new-content-in-etc \> /etc/somenewfile
vm_reboot
vm_assert_status_jq ".deployments[0][\"booted\"]" \
                    ".deployments[0][\"staged\"]|not" \
                    ".deployments[1][\"staged\"]|not" \
                    ".deployments[0][\"version\"] == \"v2\""
vm_cmd cat /etc/somenewfile > somenewfile.txt
assert_file_has_content somenewfile.txt new-content-in-etc
echo "ok autoupdate staged"
