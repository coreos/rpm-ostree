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

. ${commondir}/libtest.sh
export RPMOSTREE_SUPPRESS_REQUIRES_ROOT_CHECK=yes

ensure_dbus

echo "1..15"

setup_os_repository "archive-z2" "syslinux"

echo "ok setup"

# Note: Daemon already knows what sysroot to use, so avoid passing
#       --sysroot=sysroot to rpm-ostree commands as it will result
#       in a warning message.

OSTREE="ostree --repo=sysroot/ostree/repo"
REMOTE_OSTREE="ostree --repo=testos-repo --gpg-homedir=${test_tmpdir}/gpghome"

# This initial deployment gets kicked off with some kernel arguments 
$OSTREE remote add --set=gpg-verify=false testos file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
$OSTREE pull testos:testos/buildmaster/x86_64-runtime
ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime

assert_status_jq '.deployments[0].version == "1.0.10"'
echo "ok status shows right version"

os_repository_new_commit
rpm-ostree upgrade --os=testos

$OSTREE remote add --set=gpg-verify=false otheros file://$(pwd)/testos-repo testos/buildmaster/x86_64-runtime
rpm-ostree rebase --os=testos otheros:

assert_status_jq '.deployments[0].version == "'$(date "+%Y%m%d.0")'"'
echo "ok rebase onto newer version"

# Test 'upgrade --check' w/ no upgrade
# XXX Disabled this because source repo has no /usr/share/rpm
#     Maybe that's too heavy a requirement to just check for an
#     upgrade, but on the other hand this is *RPM*-ostree.
#rpm-ostree upgrade --os=testos --check
#test "$?" = "77" || (echo 1>&2 "Expected exit code 77, got $?"; exit 1)

# Jump backward to 1.0.9
rpm-ostree deploy --os=testos 1.0.9
assert_status_jq '.deployments[0].version == "1.0.9"'
echo "ok deploy older known version"

# Remember the current revision for later.
revision=$($OSTREE rev-parse otheros:testos/buildmaster/x86_64-runtime)

# Jump forward to a locally known version.
rpm-ostree deploy --os=testos 1.0.10
assert_status_jq '.deployments[0].version == "1.0.10"'
echo "ok deploy newer known version"

# Jump forward to a new, locally unknown version.
# Here we also test the "version=" argument prefix.
os_repository_new_commit 1 1
rpm-ostree deploy --os=testos version=$(date "+%Y%m%d.1")
assert_status_jq '.deployments[0].version == "'$(date "+%Y%m%d.1")'"'
echo "ok deploy newer unknown version"

# Jump backward again to 1.0.9, but this time using the
# "revision=" argument prefix (and test case sensitivity).
rpm-ostree deploy --os=testos REVISION=$revision
assert_status_jq '.deployments[0].version == "1.0.9"'
echo "ok deploy older version by revision"

# Make a commit on a different branch and make sure that it doesn't let us
# deploy it
other_rev=$($REMOTE_OSTREE commit -b other-branch --tree=ref=$revision)
if rpm-ostree deploy --os=testos REVISION=$other_rev 2>OUTPUT-err; then
    assert_not_reached "Deploying an out-of-branch commit unexpectedly succeeded."
fi
assert_file_has_content OUTPUT-err 'Checksum .* not found in .*'
echo "ok error on deploying commit on other branch"

# Make sure we can do an upgrade after a deploy
os_repository_new_commit 2 3
rpm-ostree upgrade --os=testos
assert_status_jq '.deployments[0].version == "'$(date "+%Y%m%d.3")'"'
echo "ok upgrade after deploy"

# Make sure we're currently on otheros
assert_json_field_in_status '.deployments[0].origin' 'otheros:*'
assert_status_jq '.deployments[0].origin|startswith("otheros:")'

os_repository_new_commit 2 2
rpm-ostree rebase --os=testos testos:testos/buildmaster/x86_64-runtime $(date "+%Y%m%d.2")
assert_status_jq '.deployments[0].origin|startswith("testos:")'
assert_status_jq '.deployments[0].version == "'$(date "+%Y%m%d.2")'"'
echo "ok rebase onto other branch at specific version"

branch=testos/buildmaster/x86_64-runtime
new_csum=$($REMOTE_OSTREE commit -b $branch --tree=ref=$branch)
rpm-ostree rebase --os=testos otheros:$branch $new_csum
assert_status_jq '.deployments[0].origin|startswith("otheros:")'
assert_status_jq '.deployments[0].checksum == "'$new_csum'"'
echo "ok rebase onto other branch at specific checksum"

if rpm-ostree rebase --os=testos testos:testos/buildmaster/x86_64-runtime $other_rev 2>OUTPUT-err; then
    assert_not_reached "Rebasing onto out-of-branch commit unexpectedly succeeded."
fi
assert_file_has_content OUTPUT-err 'Checksum .* not found in .*'
echo "ok error on rebasing onto commit on other branch"

# Make sure that we can deploy from a remote which has gone from unsigned to
# signed commits.
$REMOTE_OSTREE commit -b $branch --tree=ref=$branch \
    --gpg-sign=$TEST_GPG_KEYID --add-metadata-string version=gpg-signed
$OSTREE remote add secureos file://$(pwd)/testos-repo

rpm-ostree rebase --os=testos secureos:$branch gpg-signed
echo "ok deploy from remote with unsigned and signed commits"

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
