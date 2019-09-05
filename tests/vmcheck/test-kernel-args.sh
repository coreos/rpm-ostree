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

# XXX: uncomment this when we migrate CI to FCOS
# # Note this test is run with forced read-only sysroot on
# # https://github.com/coreos/rpm-ostree/pull/1896
# vm_cmd ostree config --repo /sysroot/ostree/repo set sysroot.readonly true
# vm_cmd systemctl restart rpm-ostreed

osname=$(vm_get_booted_deployment_info osname)

vm_kargs_now() {
    vm_rpmostree kargs "$@"
    vm_reboot
}

vm_rpmostree kargs > kargs.txt
conf_content=$(vm_cmd grep -h ^options /boot/loader/entries/ostree-*.conf | head -1 | sed -e 's,options ,,')
assert_file_has_content_literal kargs.txt "$conf_content"
echo "ok kargs display matches options"

vm_kargs_now --append=FOO=BAR --append=APPENDARG=VALAPPEND --append=APPENDARG=2NDAPPEND
# read the conf file into a txt for future comparison
vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > tmp_conf.txt
assert_file_has_content_literal tmp_conf.txt 'FOO=BAR'
assert_file_has_content_literal tmp_conf.txt 'APPENDARG=VALAPPEND APPENDARG=2NDAPPEND'

# Ensure the result flows through with rpm-ostree kargs
vm_rpmostree kargs > kargs.txt
assert_file_has_content_literal kargs.txt 'FOO=BAR'
assert_file_has_content_literal kargs.txt 'APPENDARG=VALAPPEND APPENDARG=2NDAPPEND'
echo "ok kargs append"

# Ensure the result flows through with rpm-ostree kargs
vm_rpmostree kargs --append=APPENDARG=3RDAPPEND --delete=APPENDARG=VALAPPEND
vm_rpmostree kargs > kargs.txt
assert_not_file_has_content kargs.txt 'APPENDARG=VALAPPEND'
assert_file_has_content_literal kargs.txt 'APPENDARG=3RDAPPEND'
# And reset to previous state
vm_rpmostree cleanup -p
echo "ok kargs append and delete"

# Test for rpm-ostree kargs delete
vm_kargs_now kargs --delete FOO
vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'FOO=BAR'
echo "ok delete a single key/value pair"

if vm_rpmostree kargs --delete APPENDARG 2>err.txt; then
    assert_not_reached "Delete A key with multiple values unexpectedly succeeded"
fi
assert_file_has_content err.txt "Multiple values for key 'APPENDARG' found"
echo "ok failed to delete key with multiple values"

vm_kargs_now --delete APPENDARG=VALAPPEND
vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'APPENDARG=VALAPPEND'
assert_file_has_content tmp_conf.txt 'APPENDARG=2NDAPPEND'
echo "ok delete a single key/value pair from multi valued key pairs"

# Test for rpm-ostree kargs replace
vm_status_watch_start
vm_rpmostree kargs --append=REPLACE_TEST=TEST --append=REPLACE_MULTI_TEST=TEST --append=REPLACE_MULTI_TEST=NUMBERTWO
vm_status_watch_check "Transaction: kargs --append=REPLACE_TEST=TEST --append=REPLACE_MULTI_TEST=TEST --append=REPLACE_MULTI_TEST=NUMBERTWO"

# Test for replacing key/value pair with  only one value
vm_rpmostree kargs --replace=REPLACE_TEST=HELLO
if vm_pending_is_staged; then
    vm_reboot
fi
vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > tmp_conf.txt
assert_file_has_content_literal tmp_conf.txt 'REPLACE_TEST=HELLO'
echo "ok replacing one key/value pair"

# Test for replacing key/value pair with multi vars
if vm_rpmostree kargs --replace=REPLACE_MULTI_TEST=ERR 2>err.txt; then
    assert_not_reached "Replace a key with multiple values unexpectedly succeeded"
fi
assert_file_has_content err.txt "Multiple values for key 'REPLACE_MULTI_TEST' found"
echo "ok failed to replace key with multiple values"

# Test for replacing  one of the values for multi value keys
vm_kargs_now --replace=REPLACE_MULTI_TEST=TEST=NEWTEST
vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > tmp_conf.txt
assert_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=NEWTEST"
assert_not_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=TEST"
assert_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=NUMBERTWO"
echo "ok replacing value from multi-valued key pairs"

# Test --deploy-index=1
vm_rpmostree rollback
vm_rpmostree kargs --deploy-index=1 > kargs.txt
assert_file_has_content kargs.txt 'REPLACE_MULTI_TEST=NUMBERTWO'
assert_file_has_content kargs.txt 'APPENDARG=2NDAPPEND'
echo "ok kargs correctly displayed for specific deployment indices"

# Test if the proc-cmdline option produces the same result as /proc/cmdline
vm_cmd cat /proc/cmdline > cmdlinekargs.txt
for arg in $(vm_rpmostree kargs --import-proc-cmdline | tail -n +2); do
    case "$arg" in
  ostree=*) # Skip the ostree arg due to potential boot version difference
     ;;
  *) assert_file_has_content cmdlinekargs.txt "$arg"
     ;;
    esac
done
echo "ok import kargs from current deployment"

# Test for https://github.com/projectatomic/rpm-ostree/issues/1392
vm_rpmostree kargs --append=PACKAGE=TEST
vm_build_rpm foo
vm_rpmostree install foo | tee out.txt
# make sure the string hasn't changed for the next negative check below
assert_file_has_content out.txt 'Enabled rpm-md'
vm_pending_is_staged # this is default now, but just being explicit
vm_rpmostree kargs --append=PACKAGE2=TEST2 | tee out.txt
# check that kargs modifications are done offline
assert_not_file_has_content out.txt 'Enabled rpm-md'
echo "ok kargs work offline"
vm_reboot

vm_cmd grep ^options /boot/loader/entries/ostree-2-$osname.conf > kargs.txt
assert_file_has_content_literal kargs.txt 'PACKAGE=TEST'
assert_file_has_content_literal kargs.txt 'PACKAGE2=TEST2'
echo "ok kargs with multiple operations"
vm_rpmostree kargs > kargs.txt
assert_file_has_content_literal kargs.txt 'PACKAGE=TEST'
assert_file_has_content_literal kargs.txt 'PACKAGE2=TEST2'
echo "ok kargs display with multiple operations"

# XXX: uncomment this when we migrate CI to FCOS
# # And reset this bit
# vm_cmd ostree config --repo /sysroot/ostree/repo set sysroot.readonly false
# vm_reboot
