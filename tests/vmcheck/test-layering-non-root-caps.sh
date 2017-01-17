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

# SUMMARY: check that RPM scripts are properly handled during package layering

vm_send_test_repo

# make sure the package is not already layered
vm_assert_layered_pkg nonrootcap absent

vm_rpmostree install nonrootcap
echo "ok install nonrootcap"

vm_reboot

vm_assert_layered_pkg nonrootcap present
echo "ok pkg nonrootcap added"

# let's check that the user and group were successfully added
vm_cmd getent passwd nrcuser
vm_cmd getent group nrcgroup
echo "ok user and group added"

if ! vm_has_files /usr/bin/nrc-none.sh \
                  /usr/bin/nrc-user.sh \
                  /usr/bin/nrc-group.sh \
                  /usr/bin/nrc-caps.sh \
                  /usr/bin/nrc-usergroup.sh \
                  /usr/bin/nrc-usergroupcaps.sh \
                  /var/lib/nonrootcap \
                  /run/nonrootcap; then
  assert_not_reached "not all files were layered"
fi
echo "ok all files layered"

check_user() {
  user=$(vm_cmd stat -c '%U' $1)
  if [[ $user != $2 ]]; then
    assert_not_reached "expected user $2 on file $1 but got $user"
  fi
}

check_group() {
  group=$(vm_cmd stat -c '%G' $1)
  if [[ $group != $2 ]]; then
    assert_not_reached "expected group $2 on file $1 but got $group"
  fi
}

check_fcap() {
  fcap=$(vm_cmd getcap $1)
  fcap=${fcap#* = } # trim filename
  if [[ $fcap != $2 ]]; then
    assert_not_reached "expected fcaps $2 on file $1 but got $fcap"
  fi
}

check_file() {
  check_user $1 $2
  check_group $1 $3
  check_fcap $1 $4
}

check_file /usr/bin/nrc-none.sh root root ""
check_file /usr/bin/nrc-user.sh nrcuser root ""
check_file /usr/bin/nrc-group.sh root nrcgroup ""
check_file /usr/bin/nrc-caps.sh root root "cap_net_bind_service+ep"
check_file /usr/bin/nrc-caps-setuid.sh root root "cap_net_bind_service+ep"
vm_cmd test -u /usr/bin/nrc-caps-setuid.sh
check_file /usr/bin/nrc-usergroup.sh nrcuser nrcgroup ""
check_file /usr/bin/nrc-usergroupcaps.sh nrcuser nrcgroup "cap_net_bind_service+ep"
check_file /usr/bin/nrc-usergroupcaps-setuid.sh nrcuser nrcgroup "cap_net_bind_service+ep"
vm_cmd test -u /usr/bin/nrc-usergroupcaps-setuid.sh
check_file /var/lib/nonrootcap nrcuser nrcgroup
check_file /run/nonrootcap nrcuser nrcgroup
echo "ok correct user/group and fcaps"
