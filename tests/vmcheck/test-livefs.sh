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

set -e

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

vm_assert_layered_pkg foo absent

vm_build_rpm foo
vm_rpmostree install /tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm
vm_assert_status_jq '.deployments|length == 2'
echo "ok install foo locally"

if vm_cmd rpm -q foo; then
    assert_not_reached "have foo?"
fi
assert_livefs_ok() {
    vm_rpmostree ex livefs -n > livefs-analysis.txt
    assert_file_has_content livefs-analysis.txt 'livefs OK (dry run)'
}
assert_livefs_ok

vm_assert_status_jq '.deployments|length == 2' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]|not'
vm_rpmostree ex livefs
vm_cmd rpm -q foo > rpmq.txt
assert_file_has_content rpmq.txt foo-1.0-1
vm_assert_status_jq '.deployments|length == 3' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]'

echo "ok livefs stage1"

vm_build_rpm test-livefs-with-etc \
  build 'echo "A config file for %{name}" > %{name}.conf' \
  install 'mkdir -p %{buildroot}/etc
           install %{name}.conf %{buildroot}/etc
           mkdir -p %{buildroot}/etc/%{name}/
           echo subconfig-one > %{buildroot}/etc/%{name}/subconfig-one.conf
           echo subconfig-two > %{buildroot}/etc/%{name}/subconfig-two.conf
           mkdir -p %{buildroot}/etc/%{name}/subdir
           echo subconfig-three > %{buildroot}/etc/%{name}/subdir/subconfig-three.conf
           mkdir -p %{buildroot}/etc/opt
           echo file-in-opt-subdir > %{buildroot}/etc/opt/%{name}-opt.conf' \
  files "/etc/%{name}.conf
         /etc/%{name}/*
         /etc/opt/%{name}*"

# make sure there are no config files already present
vm_cmd rm -rf /etc/test-livefs-with-etc \
              /etc/test-livefs-with-etc.conf \
              /etc/opt/test-livefs-with-etc-opt.conf

vm_rpmostree install /tmp/vmcheck/yumrepo/packages/x86_64/test-livefs-with-etc-1.0-1.x86_64.rpm
assert_livefs_ok
vm_rpmostree ex livefs
vm_cmd rpm -q foo test-livefs-with-etc > rpmq.txt
assert_file_has_content rpmq.txt foo-1.0-1 test-livefs-with-etc-1.0-1
vm_cmd cat /etc/test-livefs-with-etc.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "A config file for test-livefs-with-etc"
for v in subconfig-one subconfig-two subdir/subconfig-three; do
    vm_cmd cat /etc/test-livefs-with-etc/${v}.conf > test-livefs-with-etc.conf
    assert_file_has_content_literal test-livefs-with-etc.conf $(basename $v)
done
vm_cmd cat /etc/opt/test-livefs-with-etc-opt.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "file-in-opt-subdir"

echo "ok livefs stage2"

# Now, perform a further change in the pending
vm_rpmostree uninstall test-livefs-with-etc-1.0-1.x86_64
vm_assert_status_jq '.deployments|length == 3'
echo "ok livefs preserved rollback"

# Reset to rollback, undeploy pending
reset() {
    vm_rpmostree rollback
    vm_reboot
    vm_rpmostree cleanup -r
    vm_assert_status_jq '.deployments|length == 1' '.deployments[0]["live-replaced"]|not'
}
reset

# If the admin created a config file before, we need to keep it
vm_rpmostree install /tmp/vmcheck/yumrepo/packages/x86_64/test-livefs-with-etc-1.0-1.x86_64.rpm
vm_cmd cat /etc/test-livefs-with-etc.conf || true
vm_cmd echo custom \> /etc/test-livefs-with-etc.conf
vm_cmd cat /etc/test-livefs-with-etc.conf
vm_rpmostree ex livefs
vm_cmd cat /etc/test-livefs-with-etc.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "custom"
echo "ok livefs preserved modified config"

reset
vm_rpmostree install /tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm
vm_rpmostree ex livefs
generate_upgrade() {
    # Create a modified vmcheck commit
    cat >t.sh<<EOF
#!/bin/bash
set -xeuo pipefail
cd /ostree/repo/tmp
rm vmcheck -rf
ostree checkout vmcheck vmcheck --fsync=0
(cat vmcheck/usr/bin/ls; echo more stuff) > vmcheck/usr/bin/ls.new
mv vmcheck/usr/bin/ls{.new,}
ostree commit -b vmcheck --tree=dir=vmcheck --link-checkout-speedup
rm vmcheck -rf
EOF
    vm_cmdfile t.sh
}
generate_upgrade
# And remove the pending deployment so that our origin is now the booted
vm_rpmostree cleanup -p
vm_rpmostree upgrade
vm_assert_status_jq '.deployments|length == 3' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]'

echo "ok livefs not carried over across upgrades"

reset
generate_upgrade
vm_rpmostree upgrade
vm_assert_status_jq '.deployments|length == 2' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]|not'
if vm_rpmostree ex livefs -n &> livefs-analysis.txt; then
    assert_not_reached "livefs succeeded?"
fi
vm_assert_status_jq '.deployments|length == 2' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]|not'
assert_file_has_content livefs-analysis.txt 'live updates not currently supported for modifications'
echo "ok no modifications"
