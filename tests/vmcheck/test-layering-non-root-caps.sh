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

set -euo pipefail

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# SUMMARY: check that RPM scripts are properly handled during package layering

# make sure the package is not already layered
vm_assert_layered_pkg nonrootcap absent

vm_build_rpm nonrootcap \
    build "echo nrc.conf > nrc.conf
           for mode in none user group caps{,-setuid} usergroup{,caps{,-setuid}}; do
               cp nonrootcap nrc-\$mode.sh
           done" \
    pre "groupadd -r nrcgroup
         useradd -r nrcuser -s /sbin/nologin" \
    install "mkdir -p %{buildroot}/etc
             install nrc.conf %{buildroot}/etc
             ln -sr %{buildroot}/etc/nrc.conf %{buildroot}/etc/nrc-link.conf
             mkdir -p %{buildroot}/usr/bin
             install *.sh %{buildroot}/usr/bin
             ln -sr %{buildroot}/usr/bin/{nrc-user.sh,nrc-user-link.sh}
             mkdir -p %{buildroot}/var/lib/nonrootcap
             mkdir -p %{buildroot}/run/nonrootcap
             mkdir -p %{buildroot}/var/lib/nonrootcap-rootowned
             mkdir -p %{buildroot}/run/nonrootcap-rootowned" \
    files "/usr/bin/nrc-none.sh
           %attr(-, nrcuser, -) /etc/nrc.conf
           %attr(-, nrcuser, -) /etc/nrc-link.conf
           %ghost %attr(-, nrcuser, -) /etc/nrc-ghost.conf
           %attr(-, nrcuser, -) /usr/bin/nrc-user.sh
           %attr(-, nrcuser, -) /usr/bin/nrc-user-link.sh
           %attr(-, -, nrcgroup) /usr/bin/nrc-group.sh
           %caps(cap_net_bind_service=ep) /usr/bin/nrc-caps.sh
           %attr(4775, -, -) %caps(cap_net_bind_service=ep) /usr/bin/nrc-caps-setuid.sh
           %attr(-, nrcuser, nrcgroup) /usr/bin/nrc-usergroup.sh
           %attr(-, nrcuser, nrcgroup) %caps(cap_net_bind_service=ep) /usr/bin/nrc-usergroupcaps.sh
           %attr(4775, nrcuser, nrcgroup) %caps(cap_net_bind_service=ep) /usr/bin/nrc-usergroupcaps-setuid.sh
           %attr(-, nrcuser, nrcgroup) /var/lib/nonrootcap
           %attr(-, nrcuser, nrcgroup) /run/nonrootcap
           /var/lib/nonrootcap-rootowned
           /run/nonrootcap-rootowned"

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
                  /etc/nrc.conf \
                  /usr/bin/nrc-user.sh \
                  /usr/bin/nrc-group.sh \
                  /usr/bin/nrc-caps.sh \
                  /usr/bin/nrc-usergroup.sh \
                  /usr/bin/nrc-usergroupcaps.sh \
                  /var/lib/nonrootcap \
                  /run/nonrootcap \
                  /var/lib/nonrootcap-rootowned \
                  /run/nonrootcap-rootowned; then
  assert_not_reached "not all files were layered"
fi
echo "ok all files layered"

check_user() {
  local user=$(vm_cmd stat -c '%U' $1)
  if [[ $user != $2 ]]; then
    assert_not_reached "expected user $2 on file $1 but got $user"
  fi
}

check_group() {
  local group=$(vm_cmd stat -c '%G' $1)
  if [[ $group != $2 ]]; then
    assert_not_reached "expected group $2 on file $1 but got $group"
  fi
}

check_fcap() {
  local fcap=$(vm_cmd getcap $1)
  local fcap=${fcap#* = } # trim filename for pre-2.48 libcap: /usr/bin/foo = cap_net_raw+ep
  fcap=${fcap#* } # And from the new 2.48+ libcap: /usr/bin/foo cap_net_raw=ep
  if test -z "$2"; then
    if test -n "$fcap"; then
      assert_not_reached "expected no fcaps but found $fcap"
    fi
    return
  fi
  # Replace '+' with '='; a libcap change https://bodhi.fedoraproject.org/updates/FEDORA-2021-eeff266a64
  # changed the output, and the new variant seems more correct
  # because it's matching what we specified above.  But we need
  # to handle the previous case too for backcompat for a bit.
  fcap=${fcap/+/=}
  if [[ $fcap != $2 ]]; then
    assert_not_reached "expected fcaps $2 on file $1 but got $fcap"
  fi
}

check_file() {
  local file=$1; shift
  local user=$1; shift
  local group=$1; shift
  local fcap=${1:-}
  check_user  "$file" "$user"
  check_group "$file" "$group"
  check_fcap  "$file" "$fcap"
}

check_file /usr/bin/nrc-none.sh root root
check_file /usr/bin/nrc-user.sh nrcuser root
check_file /usr/bin/nrc-user-link.sh nrcuser root
check_file /usr/bin/nrc-group.sh root nrcgroup
check_file /usr/bin/nrc-caps.sh root root "cap_net_bind_service=ep"
check_file /usr/bin/nrc-caps-setuid.sh root root "cap_net_bind_service=ep"
vm_cmd test -u /usr/bin/nrc-caps-setuid.sh
check_file /usr/bin/nrc-usergroup.sh nrcuser nrcgroup
check_file /usr/bin/nrc-usergroupcaps.sh nrcuser nrcgroup "cap_net_bind_service=ep"
check_file /usr/bin/nrc-usergroupcaps-setuid.sh nrcuser nrcgroup "cap_net_bind_service=ep"
vm_cmd test -u /usr/bin/nrc-usergroupcaps-setuid.sh
check_file /var/lib/nonrootcap nrcuser nrcgroup
check_file /run/nonrootcap nrcuser nrcgroup
check_file /var/lib/nonrootcap-rootowned root root
check_file /run/nonrootcap-rootowned root root
check_file /etc/nrc.conf nrcuser root
check_file /etc/nrc-link.conf nrcuser root
echo "ok correct user/group and fcaps"

vm_cmd ostree fsck
echo "ok fsck"
