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

# Prepare an OSTree repo with updates
vm_ostreeupdate_prepare_reboot

# Test base ostree update
vm_ostreeupdate_create v2
# Test the update → upgrade alias
vm_rpmostree update --dry-run
# Then use the real command
vm_rpmostree upgrade
vm_reboot
vm_start_httpd ostree_server $REMOTE_OSTREE 8888
vm_rpmostree cleanup -pr
vm_assert_status_jq ".deployments[0][\"booted\"]" \
                    ".deployments[0][\"version\"] == \"v2\""
echo "ok upgrade"

vm_cmd ostree remote add otherremote --if-not-exists --no-gpg-verify http://localhost:8888/
vm_rpmostree reload
vm_rpmostree rebase otherremote:
vm_assert_status_jq ".deployments[0][\"origin\"] == \"otherremote:vmcheck\"" \
                    ".deployments[0][\"booted\"]|not"
vm_rpmostree rebase vmcheckmote:
vm_assert_status_jq ".deployments[0][\"origin\"] == \"vmcheckmote:vmcheck\"" \
                    ".deployments[0][\"booted\"]|not"
echo "ok rebase"

# A new update is available...but deploy v1 again
vm_ostreeupdate_create_noop v3
vm_rpmostree deploy v1
vm_assert_status_jq ".deployments[0][\"booted\"]|not" \
                    ".deployments[0][\"version\"] == \"v1\""
vm_rpmostree cleanup -pr
vm_rpmostree deploy version=v3
vm_assert_status_jq ".deployments[0][\"booted\"]|not" \
                    ".deployments[0][\"version\"] == \"v3\""
v3rev=$(vm_get_deployment_info 0 checksum)
vm_rpmostree deploy v1
vm_rpmostree cleanup -pr
vm_rpmostree deploy REVISION=${v3rev}
vm_assert_status_jq ".deployments[0][\"booted\"]|not" \
                    ".deployments[0][\"version\"] == \"v3\""
vm_rpmostree deploy v1
vm_reboot
vm_start_httpd ostree_server $REMOTE_OSTREE 8888
vm_rpmostree cleanup -pr
vm_rpmostree rebase otherremote:vmcheck ${v3rev}
vm_assert_status_jq ".deployments[0][\"booted\"]|not" \
                    ".deployments[0][\"version\"] == \"v3\""

echo "ok deploy"
