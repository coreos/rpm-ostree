#!/bin/bash
#
# Copyright (C) 2018 Red Hat
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

dn=$(cd $(dirname $0) && pwd)

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# This test suite enables the libostree staging feature, and executes
# the upgrade and layering-relayer tests.

export VMCHECK_FLAGS=stage-deployments
vm_cmd 'echo "[Experimental]" >> /etc/rpm-ostreed.conf'
vm_cmd 'echo StageDeployments=true >> /etc/rpm-ostreed.conf'
vm_rpmostree reload

#${dn}/test-upgrades.sh

${dn}/test-layering-relayer.sh
