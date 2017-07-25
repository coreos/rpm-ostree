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

# make sure that package-related entries are always present,
# even when they're empty
vm_assert_status_jq \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]' \
  '.deployments[0]["requested-local-packages"]' \
  '.deployments[0]["base-removals"]' \
  '.deployments[0]["requested-base-removals"]'
echo "ok empty pkg arrays in status json"

# Be sure an unprivileged user exists and that we can SSH into it. This is a bit
# underhanded, but we need a bona fide user session to verify non-priv status,
# and logging in through SSH is an easy way to achieve that.
if ! vm_cmd getent passwd testuser; then
  vm_cmd useradd testuser
  vm_cmd mkdir -pm 0700 /home/testuser/.ssh
  vm_cmd cp -a /root/.ssh/authorized_keys /home/testuser/.ssh
  vm_cmd chown -R testuser:testuser /home/testuser/.ssh
fi

# Make sure we can't layer as non-root
vm_build_rpm foo
if vm_cmd_as testuser rpm-ostree pkg-add foo &> err.txt; then
    assert_not_reached "Was able to install a package as non-root!"
fi
assert_file_has_content err.txt 'PkgChange not allowed for user'
echo "ok layering requires root or auth"

# Assert that we can do status as non-root
vm_cmd_as testuser rpm-ostree status
echo "ok status doesn't require root"

# Also check that we can do status as non-root non-active
vm_cmd runuser -u bin rpm-ostree status
echo "ok status doesn't require active PAM session"

# Add metadata string containing EnfOfLife attribtue
META_ENDOFLIFE_MESSAGE="this_is_a_test"
commit=$(vm_cmd ostree commit -b vmcheck \
            --tree=ref=vmcheck --add-metadata-string=ostree.endoflife=$META_ENDOFLIFE_MESSAGE)
vm_rpmostree upgrade
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""

# Build a layered commit and check if EndOfLife still present
vm_build_rpm foo
vm_rpmostree install foo
vm_assert_status_jq ".deployments[0][\"endoflife\"] == \"${META_ENDOFLIFE_MESSAGE}\""
