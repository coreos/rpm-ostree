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

# do a bunch of tests together so that we only have to reboot once

vm_build_rpm scriptpkg1 \
  pre "groupadd -r scriptpkg1" \
  pretrans "# http://lists.rpm.org/pipermail/rpm-ecosystem/2016-August/000391.html
            echo i should've been ignored && exit 1" \
  post_args "-p /usr/bin/python" \
  post 'open("/usr/lib/rpmostreetestinterp", "w")' \
  posttrans "# Firewalld; https://github.com/projectatomic/rpm-ostree/issues/638
             . /etc/os-release || :
             # See https://github.com/projectatomic/rpm-ostree/pull/647
             for path in /tmp /var/tmp; do
               if test -f \${path}/file-in-host-tmp-not-for-scripts; then
                 echo found file from host /tmp
                 exit 1
               fi
             done"

# check that host /tmp doesn't get mounted
vm_cmd touch /tmp/file-in-host-tmp-not-for-scripts
vm_rpmostree pkg-add scriptpkg1
echo "ok pkg-add scriptpkg1"

vm_reboot

vm_assert_layered_pkg scriptpkg1 present
echo "ok pkg scriptpkg1 added"

vm_cmd "test ! -f /usr/scriptpkg1.posttrans"
echo "ok no embarrassing crud leftover"

# let's check that the group was successfully added
vm_cmd getent group scriptpkg1
echo "ok group scriptpkg1 active"

vm_has_files "/usr/lib/rpmostreetestinterp"
echo "ok interp"

vm_build_rpm scriptpkg2 \
             post_args "-e" \
             post 'echo %%{_prefix} > /usr/lib/prefixtest.txt'
vm_build_rpm scriptpkg3 \
             post 'echo %%{_prefix} > /usr/lib/noprefixtest.txt'
vm_rpmostree pkg-add scriptpkg{2,3}
vm_rpmostree ex livefs
vm_cmd cat /usr/lib/noprefixtest.txt > noprefixtest.txt
assert_file_has_content noprefixtest.txt '%{_prefix}'
vm_cmd cat /usr/lib/prefixtest.txt > prefixtest.txt
assert_file_has_content prefixtest.txt "/usr"
echo "ok script expansion"

# And now, things that should fail
vm_build_rpm rofiles-violation \
  post "echo should fail >> /usr/share/licenses/glibc/COPYING"
if vm_rpmostree install rofiles-violation; then
    assert_not_reached "installed test-post-rofiles-violation!"
fi

# We really need a reset command to go back to the base layer
vm_rpmostree uninstall scriptpkg{1,2,3}
vm_cmd 'useradd testuser || true'
vm_cmd touch /home/testuser/somedata /tmp/sometmpfile /var/tmp/sometmpfile
vm_build_rpm rmrf post "rm --no-preserve-root -rf / &>/dev/null || true"
if vm_rpmostree install rmrf; then
    assert_not_reached "rm -rf / worked?  Uh oh."
fi
vm_cmd test -f /home/testuser/somedata -a -f /etc/fstab -a -f /tmp/sometmpfile -a -f /var/tmp/sometmpfile
