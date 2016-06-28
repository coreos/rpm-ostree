#!/bin/bash
#
# Copyright (C) 2016 Jonathan Lebon <jlebon@redhat.com>
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

# make sure package foo is not already layered
if vm_has_files /usr/bin/foo; then
  assert_not_reached "/usr/bin/foo already present"
elif vm_has_packages foo; then
  assert_not_reached "foo already in rpmdb"
elif vm_has_layered_packages foo; then
  assert_not_reached "foo already layered"
fi

vm_send /tmp/vmcheck ${commondir}/compose/yum/repo

cat > vmcheck.repo << EOF
[test-repo]
name=test-repo
baseurl=file:///tmp/vmcheck/repo
EOF

vm_send /etc/yum.repos.d vmcheck.repo

vm_cmd rpm-ostree pkg-add foo
echo "ok pkg-add foo"

vm_reboot

if ! vm_has_files /usr/bin/foo; then
  assert_not_reached "/usr/bin/foo not present"
elif ! vm_has_packages foo; then
  assert_not_reached "foo not in rpmdb"
elif ! vm_has_layered_packages foo; then
  assert_not_reached "foo not layered"
fi
echo "ok pkg foo present"

if ! vm_cmd foo | grep "Happy foobing!"; then
  assert_not_reached "foo printed wrong output"
fi
echo "ok correct output"

vm_cmd rpm-ostree rollback
vm_reboot
if vm_has_layered_packages foo; then
  assert_not_reached "foo is still layered after rollback"
fi
echo "ok rollback"
