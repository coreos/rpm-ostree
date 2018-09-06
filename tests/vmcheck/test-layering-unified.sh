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

vm_assert_layered_pkg foo absent
vm_assert_layered_pkg bar absent

vm_build_rpm foo
foo_rpm=/var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm

vm_build_rpm bar
bar_rpm=/var/tmp/vmcheck/yumrepo/packages/x86_64/bar-1.0-1.x86_64.rpm

# We cheat a bit here and don't actually reboot the system. Instead, we just
# check that then pending deployment looks sane.

# UPGRADE

commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck)
vm_rpmostree upgrade --install bar --install $foo_rpm
vm_assert_status_jq \
    ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("bar") >= 0' \
    '.deployments[0]["requested-local-packages"]|length == 1' \
    '.deployments[0]["requested-local-packages"]|index("foo-1.0-1.x86_64") >= 0'
vm_rpmostree cleanup -p

echo "ok upgrade with bar and local foo"

# DEPLOY

commit=$(vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck \
           --add-metadata-string=version=SUPADUPAVERSION)
vm_rpmostree deploy SUPADUPAVERSION --install foo --install $bar_rpm
vm_assert_status_jq \
    ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
    '.deployments[0]["version"] == "SUPADUPAVERSION"' \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("foo") >= 0' \
    '.deployments[0]["requested-local-packages"]|length == 1' \
    '.deployments[0]["requested-local-packages"]|index("bar-1.0-1.x86_64") >= 0'
vm_rpmostree cleanup -p

echo "ok deploy with foo and local bar"

# REBASE

commit=$(vm_cmd ostree commit -b vmcheck_tmp/rebase \
           --tree=ref=vmcheck --add-metadata-string=version=SUPADUPAVERSION)
vm_rpmostree rebase vmcheck_tmp/rebase SUPADUPAVERSION \
    --install bar --install $foo_rpm
vm_assert_status_jq \
    ".deployments[0][\"base-checksum\"] == \"${commit}\"" \
    '.deployments[0]["origin"] == "vmcheck_tmp/rebase"' \
    '.deployments[0]["version"] == "SUPADUPAVERSION"' \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("bar") >= 0' \
    '.deployments[0]["requested-local-packages"]|length == 1' \
    '.deployments[0]["requested-local-packages"]|index("foo-1.0-1.x86_64") >= 0'
vm_rpmostree cleanup -p

echo "ok rebase with bar and local foo"

# PKG CHANGES

vm_rpmostree install $foo_rpm
vm_assert_status_jq \
    '.deployments[0]["packages"]|length == 0' \
    '.deployments[0]["requested-local-packages"]|length == 1' \
    '.deployments[0]["requested-local-packages"]|index("foo-1.0-1.x86_64") >= 0'
vm_rpmostree uninstall foo-1.0-1.x86_64 --install bar
vm_assert_status_jq \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("bar") >= 0' \
    '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree install foo --uninstall bar
vm_assert_status_jq \
    '.deployments[0]["packages"]|length == 1' \
    '.deployments[0]["packages"]|index("foo") >= 0' \
    '.deployments[0]["requested-local-packages"]|length == 0'
vm_rpmostree cleanup -p

echo "ok simultaneous pkg changes"
