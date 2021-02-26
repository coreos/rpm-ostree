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

vm_cmd rpm -qa | sort > original-rpmdb.txt

vm_build_rpm foo
vm_build_rpm bar
vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm
vm_assert_status_jq '.deployments|length == 2'
echo "ok install foo locally"

if vm_cmd rpm -q foo; then
    assert_not_reached "have foo?"
fi

vm_rpmostree status > status.txt
assert_not_file_has_content_literal status.txt 'LiveDiff'
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
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'LiveDiff: 1 added'
vm_rpmostree status -v > status.txt
assert_file_has_content_literal status.txt 'LiveAdded:'
echo "ok livefs basic"

vm_rpmostree cleanup -p
vm_rpmostree install bar
vm_assert_status_jq '.deployments|length == 2' \
                    '.deployments[0]["live-replaced"]|not' \
                    '.deployments[1]["live-replaced"]'
vm_rpmostree ex livefs | tee out.txt
assert_file_has_content out.txt 'Added:'
assert_file_has_content out.txt '  bar-1.0'
vm_cmd rpm -qa > rpmq.txt
assert_file_has_content rpmq.txt bar-1.0-1
assert_not_file_has_content rpmq.txt foo-1.0-1
vm_cmd ls -al /usr/bin/bar
if vm_cmd test -f /usr/bin/foo; then
    fatal "Still have /usr/bin/foo"
fi
vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'LiveDiff: 1 added'

echo "ok livefs again"

vm_build_rpm test-livefs-with-etc \
  build 'echo "A config file for %{name}" > %{name}.conf' \
  install 'mkdir -p %{buildroot}/etc
           install %{name}.conf %{buildroot}/etc
           echo otherconf > %{buildroot}/etc/%{name}-other.conf
           mkdir -p %{buildroot}/etc/%{name}/
           echo subconfig-one > %{buildroot}/etc/%{name}/subconfig-one.conf
           echo subconfig-two > %{buildroot}/etc/%{name}/subconfig-two.conf
           mkdir -p %{buildroot}/etc/%{name}/subdir
           install -d %{buildroot}/etc/%{name}/subdir/subsubdir
           echo subconfig-three > %{buildroot}/etc/%{name}/subdir/subsubdir/subconfig-three.conf
           ln -s / %{buildroot}/etc/%{name}/subdir/link2root
           ln -s nosuchfile %{buildroot}/etc/%{name}/link2nowhere
           ln -s . %{buildroot}/etc/%{name}/subdir/link2self
           ln -s ../.. %{buildroot}/etc/%{name}/subdir/link2parent
           mkdir -p %{buildroot}/etc/opt
           echo file-in-opt-subdir > %{buildroot}/etc/opt/%{name}-opt.conf' \
  files "/etc/%{name}.conf
         /etc/%{name}-other.conf
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
# But test with a modified config file
vm_cmd echo myconfig \> /etc/test-livefs-with-etc-other.conf
vm_cmd grep myconfig /etc/test-livefs-with-etc-other.conf

vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/test-livefs-{with-etc,service}-1.0-1.x86_64.rpm
vm_rpmostree ex apply-live
vm_cmd rpm -q bar test-livefs-{with-etc,service} > rpmq.txt
assert_file_has_content rpmq.txt bar-1.0-1 test-livefs-{with-etc,service}-1.0-1
vm_cmd cat /etc/test-livefs-with-etc.conf > test-livefs-with-etc.conf
assert_file_has_content test-livefs-with-etc.conf "A config file for test-livefs-with-etc"
vm_cmd cat /etc/test-livefs-with-etc-other.conf > conf
assert_file_has_content conf myconfig
for v in subconfig-one subconfig-two subdir/subsubdir/subconfig-three; do
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

vm_rpmostree status > status.txt
assert_file_has_content_literal status.txt 'LiveDiff: 3 added'

echo "ok livefs stage2"

# Now undo it all
vm_rpmostree ex apply-live --reset
vm_cmd rpm -qa | sort > current-rpmdb.txt
diff -u original-rpmdb.txt current-rpmdb.txt
if vm_cmd test -f /usr/bin/bar; then
    fatal "Still have /usr/bin/bar"
fi
vm_rpmostree status > status.txt
assert_not_file_has_content_literal status.txt 'LiveDiff:'

echo "ok livefs reset"

# Validate that we can generate a local ostree commit
# that adds content, but doesn't change any packages -
# i.e. there's no package diff.  This is a bit of a corner
# case in various bits of the code.
booted=$(vm_get_booted_csum)
vm_shell_inline_sysroot_rw <<EOF
ostree refs --create "localref" ${booted}
cd \$(mktemp -d)
mkdir -p usr/share/localdata
echo mytestdata > usr/share/localdata/mytestfile
ostree commit --base=localref --selinux-policy-from-base -b localref --tree=dir=.
rpm-ostree rebase :localref
rpm-ostree ex apply-live
EOF
vm_cmd cat /usr/share/localdata/mytestfile > out.txt
assert_file_has_content out.txt mytestdata
echo "ok local ref without package changes"
