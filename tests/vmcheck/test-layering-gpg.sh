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

vm_clean_caches

# make sure the package is not already layered
vm_assert_layered_pkg foo absent

vm_build_rpm_repo_mode gpgcheck foo version 4.5 release 6
if vm_rpmostree pkg-add foo-4.5 2>err.txt; then
    assert_not_reached "Installed unsigned package"
fi
assert_file_has_content err.txt 'cannot be verified'
echo "ok failed to install unsigned package"
