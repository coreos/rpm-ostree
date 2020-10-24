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

# We do various assertions on deployment length, need a reliably
# clean slate.
vm_rpmostree cleanup -pr

vm_assert_layered_pkg foo absent

vm_build_rpm foo
vm_build_rpm bar
vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm
vm_assert_status_jq '.deployments|length == 2'
echo "ok install foo locally"

if vm_cmd rpm -q foo; then
    assert_not_reached "have foo?"
fi

vm_assert_status_jq '.deployments|length == 2' \
                    '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]|not'
vm_rpmostree ex livefs
vm_cmd rpm -q foo > rpmq.txt
assert_file_has_content rpmq.txt foo-1.0-1
vm_cmd ls -al /usr/bin/foo
vm_assert_status_jq '.deployments|length == 2' '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]'
if vm_cmd test -w /usr; then
    fatal "Found writable /usr"
fi
echo "ok livefs basic"

vm_rpmostree cleanup -p
vm_rpmostree install bar
vm_assert_status_jq '.deployments|length == 2' \
                    '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]'
vm_rpmostree ex livefs
vm_cmd rpm -qa > rpmq.txt
assert_file_has_content rpmq.txt bar-1.0-1
assert_not_file_has_content rpmq.txt foo-1.0-1
vm_cmd ls -al /usr/bin/bar
if vm_cmd test -f /usr/bin/foo; then
    fatal "Still have /usr/bin/foo"
fi

echo "ok livefs again"

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

# Simulate a service that adds a user and has data in tmpfiles.d
vm_build_rpm test-livefs-service \
             build "echo test-livefs-service > test-livefs-service.txt" \
             install "mkdir -p %{buildroot}/{usr/share,var/lib/%{name}}
                      install test-livefs-service.txt %{buildroot}/usr/share" \
             pre "groupadd -r livefs-group
                  useradd -r livefs-user -g livefs-group -s /sbin/nologin" \
             files "/usr/share/%{name}.txt
                    /var/lib/%{name}"

# make sure there are no config files already present
vm_cmd rm -rf /etc/test-livefs-with-etc \
              /etc/test-livefs-with-etc.conf \
              /etc/opt/test-livefs-with-etc-opt.conf

vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/test-livefs-{with-etc,service}-1.0-1.x86_64.rpm
vm_rpmostree ex livefs
vm_cmd rpm -q bar test-livefs-{with-etc,service} > rpmq.txt
assert_file_has_content rpmq.txt bar-1.0-1 test-livefs-{with-etc,service}-1.0-1
vm_cmd cat /etc/test-livefs-with-etc.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "A config file for test-livefs-with-etc"
for v in subconfig-one subconfig-two subdir/subconfig-three; do
    vm_cmd cat /etc/test-livefs-with-etc/${v}.conf > test-livefs-with-etc.conf
    assert_file_has_content_literal test-livefs-with-etc.conf $(basename $v)
done
vm_cmd cat /etc/opt/test-livefs-with-etc-opt.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "file-in-opt-subdir"
# Test /usr/lib/{passwd,group} bits
vm_cmd getent passwd livefs-user > test-livefs-user.txt
assert_file_has_content test-livefs-user.txt livefs-user
vm_cmd getent group livefs-group > test-livefs-group.txt
assert_file_has_content test-livefs-group.txt livefs-group
# Test systemd-tmpfiles
vm_cmd test -d /var/lib/test-livefs-service

echo "ok livefs stage2"
