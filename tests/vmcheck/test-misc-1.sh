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

vm_rpmostree --version > version.yaml
python3 -c 'import yaml; v=yaml.safe_load(open("version.yaml")); assert("Version" in v["rpm-ostree"])'
echo "ok yaml version"

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

wrapdir="/usr/libexec/rpm-ostree/wrapped"
if [ -d "${wrapdir}" ]; then
    # Test wrapped functions for rpm
    rpm --version
    rpm -qa > /dev/null
    rpm --verify >out.txt
    assert_file_has_content out.txt "rpm --verify is not necessary for ostree-based systems"
    rm -f out.txt
    if rpm -e bash 2>out.txt; then
        fatal "rpm -e worked"
    fi
    assert_file_has_content out.txt 'Dropping privileges as `rpm` was executed with not "known safe" arguments'

    if dracut --blah 2>out.txt; then
        fatal "dracut worked"
    fi
    assert_file_has_content out.txt 'This system is rpm-ostree based'
    rm -f out.txt
else
    echo "Missing ${wrapdir}; cliwrap not enabled"
fi

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
