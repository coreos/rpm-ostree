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

# make sure there's nothing yet
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not'

# check that pkgs that install sepolicies have their changes take effect
vm_build_selinux_rpm foobar-selinux /usr/bin/foobar install_exec_t
vm_build_rpm foobar requires foobar-selinux
vm_rpmostree install foobar
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]' \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["packages"]|index("foobar") >= 0'

assert_expected_label() {
    local path=$1; shift
    local expected=system_u:object_r:$1:s0; shift
    local l=$(vm_cmd matchpathcon -n $path)
    assert_streq "$l" "$expected"
}

assert_actual_label() {
    local path=$1; shift
    local expected=system_u:object_r:$1:s0; shift
    local l=$(vm_cmd getfattr -n security.selinux --absolute-names --only-values $path)
    assert_streq "$l" "$expected"
}

# shouldn't have affected our current policy
assert_expected_label /usr/bin/foobar bin_t

# but should have affected the new root
root=$(vm_get_deployment_root 0)
assert_actual_label $root/usr/bin/foobar install_exec_t
echo "ok layer selinux pkg"

# now let's change the policy
vm_build_selinux_rpm foobar-selinux /usr/bin/foobar shell_exec_t
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck --bootable
vm_rpmostree upgrade
root=$(vm_get_deployment_root 0)
assert_actual_label $root/usr/bin/foobar shell_exec_t
echo "ok upgrade selinux pkg"

# check that a change in the base layer binary policy causes a relabel
# we do this by just baking in a layered RPM that recompiles the policy
vm_rpmostree cleanup -p
vm_build_selinux_rpm baz-selinux /usr/bin/baz install_exec_t
vm_rpmostree install baz-selinux
se_csum=$(vm_cmd ostree checksum /usr/etc/selinux/targeted/policy/policy.*)
root=$(vm_get_deployment_root 0)
se_new_csum=$(vm_cmd ostree checksum $root/usr/etc/selinux/targeted/policy/policy.*)
assert_not_streq "$se_csum" "$se_new_csum"
csum=$(vm_get_deployment_info 0 checksum)
vm_cmd ostree commit -b vmcheck --tree=ref=$csum --bootable
vm_rpmostree cleanup -p
echo "ok setup relabel"

# now we have a pending upgrade with a different sepolicy
# let's install some packages before upgrading
vm_build_rpm baz
vm_rpmostree install baz
root=$(vm_get_deployment_root 0)
assert_actual_label $root/usr/bin/baz bin_t
cursor=$(vm_get_journal_cursor)
vm_rpmostree upgrade
vm_assert_journal_has_content $cursor 'Relabeled 1/1 pkgs'
root=$(vm_get_deployment_root 0)
assert_actual_label $root/usr/bin/baz install_exec_t
echo "ok relabel"
