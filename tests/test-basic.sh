#!/bin/bash
#
# Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

. $(dirname $0)/libtest.sh

check_root_test

echo "1..9"

setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

# Note: Daemon already knows what sysroot to use, so avoid passing
#       --sysroot=sysroot to rpm-ostree commands as it will result
#       in a warning message.

# This initial deployment gets kicked off with some kernel arguments 
ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
ostree --repo=sysroot/ostree/repo pull testos:testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime

rpm-ostree status | tee OUTPUT-status.txt

assert_file_has_content OUTPUT-status.txt '1\.0\.10'
echo "ok status shows right version"

os_repository_new_commit
rpm-ostree upgrade --os=testos

ostree --repo=sysroot/ostree/repo remote add --set=gpg-verify=false otheros file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
rpm-ostree rebase --os=testos otheros:

rpm-ostree status | tee OUTPUT-status.txt

assert_not_file_has_content OUTPUT-status.txt '1\.0\.10'
version=$(date "+%Y%m%d\.0")
assert_file_has_content OUTPUT-status.txt $version
echo "ok rebase onto newer version"

# Test 'upgrade --check' w/ no upgrade
# XXX Disabled this because source repo has no /usr/share/rpm
#     Maybe that's too heavy a requirement to just check for an
#     upgrade, but on the other hand this is *RPM*-ostree.
#rpm-ostree upgrade --os=testos --check
#test "$?" = "77" || (echo 1>&2 "Expected exit code 77, got $?"; exit 1)

# Jump backward to 1.0.9
rpm-ostree deploy --os=testos 1.0.9
rpm-ostree status | head --lines 3 | tee OUTPUT-status.txt
assert_file_has_content OUTPUT-status.txt '1\.0\.9'
echo "ok deploy older known version"

# Remember the current revision for later.
revision=$(ostree rev-parse --repo=sysroot/ostree/repo otheros:testos/buildmaster/x86_64-runtime)

# Jump forward to a locally known version.
rpm-ostree deploy --os=testos 1.0.10
rpm-ostree status | head --lines 3 | tee OUTPUT-status.txt
assert_file_has_content OUTPUT-status.txt '1\.0\.10'
echo "ok deploy newer known version"

# Jump forward to a new, locally unknown version.
# Here we also test the "version=" argument prefix.
os_repository_new_commit 1 1
rpm-ostree deploy --os=testos version=$(date "+%Y%m%d.1")
rpm-ostree status | head --lines 3 | tee OUTPUT-status.txt
assert_file_has_content OUTPUT-status.txt $(date "+%Y%m%d\.1")
echo "ok deploy newer unknown version"

# Jump backward again to 1.0.9, but this time using the
# "revision=" argument prefix (and test case sensitivity).
rpm-ostree deploy --os=testos REVISION=$revision
rpm-ostree status | head --lines 3 | tee OUTPUT-status.txt
assert_file_has_content OUTPUT-status.txt '1\.0\.9'
echo "ok deploy older version by revision"

# Ensure it returns an error when passing a wrong option.
rpm-ostree --help | awk '/^$/ {in_commands=0} {if(in_commands==1){print $0}} /^Builtin Commands:/ {in_commands=1}' > commands
while read command; do
    if rpm-ostree $command --n0t-3xisting-0ption &>/dev/null; then
        assert_not_reached "command $command --n0t-3xisting-0ption was successful"
    fi
done < commands
echo "ok error on unknown command options"

if rpm-ostree nosuchcommand --nosuchoption 2>err.txt; then
    assert_not_reached "Expected an error for nosuchcommand"
fi
assert_file_has_content err.txt 'Unknown.*command'
echo "ok error on unknown command"
