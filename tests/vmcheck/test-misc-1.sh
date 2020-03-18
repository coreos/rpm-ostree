#!/bin/bash
#
# Copyright (C) 2017,2018 Red Hat, Inc.
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

# Miscellaneous basic tests; most are nondestructive

# https://github.com/projectatomic/rpm-ostree/issues/1301
# FIXME: temporarily disabled as it really wants to start
# from a fresh instance and we don't currently guarantee that.
#
# But we'll rework the test suite to do that soon like
# https://github.com/ostreedev/ostree/pull/1462
# vm_cmd 'mv /etc/ostree/remotes.d{,.orig}'
# vm_cmd systemctl restart rpm-ostreed
# vm_cmd rpm-ostree status > status.txt
# assert_file_has_content status.txt 'Remote.*not found'
# vm_cmd 'mv /etc/ostree/remotes.d{.orig,}'
# vm_rpmostree reload
# echo "ok remote not found"

# make sure that package-related entries are always present,
# even when they're empty
vm_assert_status_jq \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]' \
  '.deployments[0]["requested-local-packages"]' \
  '.deployments[0]["base-removals"]' \
  '.deployments[0]["requested-base-removals"]' \
  '.deployments[0]["layered-commit-meta"]|not'
echo "ok empty pkg arrays, and commit meta correct in status json"

vm_rpmostree status --jsonpath '$.deployments[0].booted' > jsonpath.txt
assert_file_has_content_literal jsonpath.txt 'true'
echo "ok jsonpath"

vm_rpmostree --version > version.yaml
python3 -c 'import yaml; v=yaml.safe_load(open("version.yaml")); assert("Version" in v["rpm-ostree"])'
echo "ok yaml version"

# Ensure we return an error when passing a wrong option.
vm_rpmostree --help | awk '/^$/ {in_commands=0} {if(in_commands==1){print $0}} /^Builtin Commands:/ {in_commands=1}' > commands
while read command; do
    if vm_rpmostree $command --n0t-3xisting-0ption &>/dev/null; then
        assert_not_reached "command $command --n0t-3xisting-0ption was successful"
    fi
done < commands
echo "ok error on unknown command options"

if vm_rpmostree nosuchcommand --nosuchoption 2>err.txt; then
    assert_not_reached "Expected an error for nosuchcommand"
fi
assert_file_has_content err.txt 'Unknown.*command'
echo "ok error on unknown command"

# Make sure we can't do various operations as non-root
vm_build_rpm foo
if vm_cmd_as core rpm-ostree pkg-add foo &> err.txt; then
    assert_not_reached "Was able to install a package as non-root!"
fi
assert_file_has_content err.txt 'PkgChange not allowed for user'
if vm_cmd_as core rpm-ostree reload &> err.txt; then
    assert_not_reached "Was able to reload as non-root!"
fi
assert_file_has_content err.txt 'ReloadConfig not allowed for user'
echo "ok auth"

# Test coreos-rootfs
vm_shell_inline > coreos-rootfs.txt << EOF
    mkdir /var/tmp/coreos-rootfs
    rpm-ostree coreos-rootfs seal /var/tmp/coreos-rootfs
    lsattr -d /var/tmp/coreos-rootfs
    rpm-ostree coreos-rootfs seal /var/tmp/coreos-rootfs
EOF
assert_file_has_content coreos-rootfs.txt '-*i-* /var/tmp/coreos-rootfs'

# Assert that we can do status as non-root
vm_cmd_as core rpm-ostree status
echo "ok status doesn't require root"

# StateRoot is only in --verbose
vm_rpmostree status > status.txt
assert_not_file_has_content status.txt StateRoot:
vm_rpmostree status -v > status.txt
assert_file_has_content status.txt StateRoot:
echo "ok status text"

# Also check that we can do status as non-root non-active
vm_cmd runuser -u bin rpm-ostree status
echo "ok status doesn't require active PAM session"

vm_rpmostree status -b > status.txt
assert_streq $(grep -F -e 'ostree://' status.txt | wc -l) "1"
assert_file_has_content status.txt BootedDeployment:
echo "ok status -b"

# Reload as root https://github.com/projectatomic/rpm-ostree/issues/976
vm_cmd rpm-ostree reload
echo "ok reload"

stateroot=$(vm_get_booted_stateroot)
ospath=/org/projectatomic/rpmostree1/${stateroot//-/_}
# related: https://github.com/coreos/fedora-coreos-config/issues/194
vm_cmd env LANG=C.UTF-8 gdbus call \
  --system --dest org.projectatomic.rpmostree1 \
  --object-path /org/projectatomic/rpmostree1/fedora_coreos \
  --method org.projectatomic.rpmostree1.OSExperimental.Moo true > moo.txt
assert_file_has_content moo.txt '🐄'
echo "ok moo"

vm_rpmostree usroverlay
vm_cmd test -w /usr/bin
echo "ok usroverlay"

vm_shell_inline_sysroot_rw <<EOF
    rpm-ostree cleanup -p
    originpath=\$(ostree admin --print-current-dir).origin
    cp -a \${originpath}{,.orig}
    echo "unconfigured-state=Access to TestOS requires ONE BILLION DOLLARS" >> \${originpath}
    rpm-ostree reload
    rpm-ostree status
    if rpm-ostree upgrade 2>err.txt; then
       echo "Upgraded from unconfigured-state"
       exit 1
    fi
    grep -qFe 'ONE BILLION DOLLARS' err.txt
    mv \${originpath}{.orig,}
    rpm-ostree reload
EOF
echo "ok unconfigured status"
