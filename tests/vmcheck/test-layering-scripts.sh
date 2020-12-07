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

# do a bunch of tests together so that we only have to reboot once

# For the expected semantics of these scripts, see the comments in
# rpmostree-scripts.c.
vm_build_rpm scriptpkg1 \
  pre "groupadd -r scriptpkg1" \
  pretrans "# http://lists.rpm.org/pipermail/rpm-ecosystem/2016-August/000391.html
            echo pretrans should've been ignored && exit 1" \
  verifyscript "echo verifyscript should've been ignored && exit 1" \
  post_args "-p /usr/bin/bash" \
  post '
# default shell is sh, but we requested bash; check that rpm-ostree picks it up
interp=$(cat /proc/$$/comm)
if [ "$interp" != "bash" ]; then
  echo "Expected bash interpreter, got $interp"
  exit 1
fi
touch /usr/lib/rpmostreetestinterp
touch /var/lib/rpm-state/scriptpkg1-stamp' \
  posttrans "# Firewalld; https://github.com/projectatomic/rpm-ostree/issues/638
             . /etc/os-release || :
             # See https://github.com/projectatomic/rpm-ostree/pull/647
             for path in /tmp /var/tmp; do
               if test -f \${path}/file-in-host-tmp-not-for-scripts; then
                 echo found file from host /tmp
                 exit 1
               fi
             done;
             test -f /var/lib/rpm-state/scriptpkg1-stamp"

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
# cleanup
vm_rpmostree uninstall scriptpkg1
vm_reboot

# post ordering
vm_build_rpm postorder1 \
             post 'touch /usr/share/postorder1.post' \
             posttrans 'test -f /usr/share/postorder1.post && test -f /usr/share/postorder2.post'
vm_build_rpm postorder2 \
             requires 'postorder1' \
             post 'touch /usr/share/postorder2.post' \
             posttrans 'test -f /usr/share/postorder1.post && test -f /usr/share/postorder2.post'
vm_rpmostree install postorder{1,2}
vm_rpmostree cleanup -p
echo "ok post ordering"

# lua should (currently) fail
vm_build_rpm luapkg \
             post_args "-p <lua>" \
             post 'posix.stat("/")'
if vm_rpmostree install luapkg 2>err.txt; then
    assert_not_reached "lua post?"
fi
assert_file_has_content_literal err.txt "unsupported <lua> script in '%post'"
echo "ok lua %post"

# script expansion
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

# script overrides (also one that expands)
vm_build_rpm rpmostree-lua-override-test \
             post_args "-p <lua>" \
             post 'posix.stat("/")'
vm_build_rpm rpmostree-lua-override-test-expand \
             post_args "-e -p <lua>" \
             post 'posix.stat("/")'
vm_rpmostree install rpmostree-lua-override-test{,-expand}
vm_rpmostree ex livefs
vm_cmd cat /usr/share/rpmostree-lua-override-test > lua-override.txt
assert_file_has_content lua-override.txt _install_langs
vm_cmd rpm --eval '%{_install_langs}' > install-langs.txt
vm_cmd cat /usr/share/rpmostree-lua-override-test-expand > lua-override-expand.txt
diff -u install-langs.txt lua-override-expand.txt
echo "ok script override"

vm_rpmostree reset
vm_reboot
vm_rpmostree cleanup -pr
# File triggers are Fedora+
if ! vm_cmd grep -q 'ID=.*centos' /etc/os-release; then
# We use /usr/share/licenses since it's small predictable content
license_combos="zlib-rpm systemd-tar-rpm sed-tzdata"
license_un_combos="zlib systemd-rpm"
vm_build_rpm scriptpkg4 \
             transfiletriggerin "/usr/share/licenses/zlib /usr/share/licenses/rpm" 'sort >/usr/share/transfiletriggerin-license-zlib-rpm.txt' \
             transfiletriggerun "/usr/share/licenses/zlib" 'sort >/usr/share/transfiletriggerun-license-zlib.txt'
vm_build_rpm scriptpkg5 \
             transfiletriggerin "/usr/share/licenses/systemd /usr/share/licenses/rpm /usr/share/licenses/tar" 'sort >/usr/share/transfiletriggerin-license-systemd-tar-rpm.txt' \
             transfiletriggerun "/usr/share/licenses/systemd /usr/share/licenses/rpm" 'sort >/usr/share/transfiletriggerun-license-systemd-rpm.txt' \
             transfiletriggerin2 "/usr/share/licenses/sed /usr/share/licenses/tzdata" 'sort >/usr/share/transfiletriggerin-license-sed-tzdata.txt'
vm_rpmostree pkg-add scriptpkg{4,5}
vm_rpmostree ex livefs
for combo in ${license_combos}; do
    vm_cmd cat /usr/share/transfiletriggerin-license-${combo}.txt > transfiletriggerin-license-${combo}.txt
    rm -f transfiletriggerin-fs-${combo}.txt.tmp
    (for path in $(echo ${combo} | sed -e 's,-, ,g'); do
         vm_cmd find /usr/share/licenses/${path} -type f
     done) | sort > transfiletriggerin-fs-license-${combo}.txt
    diff -u transfiletriggerin-license-${combo}.txt transfiletriggerin-fs-license-${combo}.txt
done
for combo in ${license_un_combos}; do
    vm_cmd test '!' -f /usr/share/licenses/transfiletriggerun-license-${combo}.txt
done
# We really need a reset command to go back to the base layer
vm_rpmostree uninstall scriptpkg{4,5}
echo "ok transfiletriggerin"
fi

# Should work now that we're using --copyup
# https://github.com/projectatomic/rpm-ostree/pull/1171
vm_build_rpm rofiles-copyup \
             post "echo XXXcopyupXXX >> /usr/share/rpm-ostree/treefile.json"
vm_rpmostree install rofiles-copyup
vm_cmd cat $(vm_get_deployment_root 0)/usr/share/rpm-ostree/treefile.json > out.txt
assert_file_has_content out.txt "XXXcopyupXXX"
# use posttrans to switch things up
vm_build_rpm rofiles-copyup-overwrite requires rofiles-copyup \
             posttrans "echo XXXoverwriteXXX > /usr/share/rpm-ostree/treefile.json"
vm_rpmostree install rofiles-copyup-overwrite
vm_cmd cat $(vm_get_deployment_root 0)/usr/share/rpm-ostree/treefile.json > out.txt
assert_file_has_content out.txt "XXXoverwriteXXX"
assert_not_file_has_content out.txt "XXXcopyupXXX"
vm_rpmostree uninstall rofiles-copyup rofiles-copyup-overwrite
echo "ok copyup scriptlets"

# Test cancellation via having a script hang; we interrupt directly by sending
# SIGINT to the client binary.
vm_build_rpm post-that-hangs \
             post "echo entering post-that-hangs-infloop 1>&2; while true; do sleep 1h; done"
background_install_post_that_hangs() {
    local cursor="$1"
    # use a systemd transient service as an easy way to run in the background; be
    # sure any previous failed instances are cleaned up
    vm_cmd systemctl stop vmcheck-install-hang || true
    vm_cmd systemctl reset-failed vmcheck-install-hang || true
    vm_cmd systemd-run --unit vmcheck-install-hang rpm-ostree install post-that-hangs
    if ! vm_wait_content_after_cursor "${cursor}" "entering post-that-hangs-infloop"; then
        vm_cmd systemctl stop vmcheck-install-hang || true
        assert_not_reached "failed to wait for post-that-hangs"
    fi
}
cursor=$(vm_get_journal_cursor)
background_install_post_that_hangs "${cursor}"
vm_cmd pkill --signal INT -f "'rpm-ostree install post-that-hangs'"
# Wait for our expected result
vm_wait_content_after_cursor "${cursor}" "Txn.*failed.*Running %post for post-that-hangs"
# Forcibly restart now to avoid any races with the txn finally exiting
vm_cmd systemctl restart rpm-ostreed
echo "ok cancel infinite post via SIGINT"

# Test `rpm-ostree cancel` (which is the same as doing a Ctrl-C on the client)
cursor=$(vm_get_journal_cursor)
background_install_post_that_hangs "${cursor}"
vm_rpmostree cancel
vm_wait_content_after_cursor "${cursor}" "Txn.*failed.*Running %post for post-that-hangs"
# Forcibly restart now to avoid any races with the txn finally exiting
vm_cmd systemctl restart rpm-ostreed
echo "ok cancel infinite post via `rpm-ostree cancel`"

# Test rm -rf /!
vm_cmd touch /home/core/somedata /tmp/sometmpfile /var/tmp/sometmpfile
vm_build_rpm rmrf post "rm --no-preserve-root -rf / &>/dev/null || true"
if vm_rpmostree install rmrf 2>err.txt; then
    assert_not_reached "rm -rf / worked?  Uh oh."
fi
vm_cmd test -f /home/core/somedata -a -f /etc/passwd -a -f /tmp/sometmpfile -a -f /var/tmp/sometmpfile
# This is the error today, we may improve it later
assert_file_has_content err.txt 'error: Sanity-checking final rootfs: Executing bwrap(/usr/bin/true)'
echo "ok impervious to rm -rf post"

cursor=$(vm_get_journal_cursor)
vm_build_rpm post-query-rpmdb post "rpm -q bash"
if vm_rpmostree install post-query-rpmdb; then
  assert_not_reached "Ran rpm -q bash in %post"
fi
vm_assert_journal_has_content "$cursor" 'rpm-ostree(post-query-rpmdb.post).*package bash is not installed'
echo "ok post that calls rpm -q"

# capabilities
vm_build_rpm test-cap-drop post "capsh --print > /usr/share/rpmostree-capsh.txt"
vm_rpmostree install test-cap-drop
vm_rpmostree ex livefs
vm_cmd cat /usr/share/rpmostree-capsh.txt > caps.txt
assert_not_file_has_content caps.test '^Current: =.*cap_sys_admin'

# See also rofiles-copyup above
vm_build_rpm etc-mutate post "truncate -s 0 /etc/selinux/config"
vm_rpmostree install etc-mutate
vm_rpmostree uninstall etc-mutate

# SYSTEMD_OFFLINE
vm_build_rpm test-systemd-offline post 'test "${SYSTEMD_OFFLINE}" = 1'
vm_rpmostree install test-systemd-offline
vm_rpmostree uninstall test-systemd-offline

# Ensure this is reset; at least in the Vagrant box with
# fedora-atomic:fedora/26/x86_64/atomic-host
# Version: 26.131 (2017-09-19 22:29:04)
# Commit: 98088cb6ed2a4b3f7e4e7bf6d34f9e137c296bc43640b4c1967631f22fe1802f
# it starts out modified - the modification is just deletion of trailing
# whitespace.
vm_cmd cp /{usr/,}etc/selinux/config
vm_build_rpm etc-copy post "cp /etc/selinux/config{,.new}
                            echo '# etc-copy comment' >> /etc/selinux/config.new
                            mv /etc/selinux/config{.new,}"
vm_rpmostree install etc-copy
root=$(vm_get_deployment_root 0)
vm_cmd cat $root/etc/selinux/config > new-config.txt
assert_file_has_content new-config.txt etc-copy
vm_rpmostree cleanup -p
echo "ok etc rofiles"
