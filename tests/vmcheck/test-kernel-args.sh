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

osname=$(vm_get_booted_deployment_info osname)

vm_rpmostree ex kargs > kargs.txt
conf_content=$(vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf | sed -e 's,options ,,')
assert_file_has_content_literal kargs.txt "$conf_content"
echo "ok kargs display matches options"

vm_rpmostree ex kargs --append=FOO=BAR --append=APPENDARG=VALAPPEND --append=APPENDARG=2NDAPPEND
# read the conf file into a txt for future comparison
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_file_has_content_literal tmp_conf.txt 'FOO=BAR'
assert_file_has_content_literal tmp_conf.txt 'APPENDARG=VALAPPEND APPENDARG=2NDAPPEND'

# ensure the result flows through with rpm-ostree ex kargs
vm_rpmostree ex kargs > kargs.txt
assert_file_has_content_literal kargs.txt 'FOO=BAR'
assert_file_has_content_literal kargs.txt 'APPENDARG=VALAPPEND APPENDARG=2NDAPPEND'
echo "ok kargs append"

# test for rpm-ostree ex kargs delete
vm_rpmostree ex kargs --delete FOO
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'FOO=BAR'
echo "ok delete a single key/value pair"

if vm_rpmostree ex kargs --delete APPENDARG 2>err.txt; then
    assert_not_reached "Delete A key with multiple values unexpectedly succeeded"
fi
assert_file_has_content err.txt "Unable to delete argument 'APPENDARG' with multiple values"
echo "ok failed to delete key with multiple values"

vm_rpmostree ex kargs --delete APPENDARG=VALAPPEND
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'APPENDARG=VALAPPEND'
assert_file_has_content tmp_conf.txt 'APPENDARG=2NDAPPEND'
echo "ok delete a single key/value pair from multi valued key pairs"

# test for rpm-ostree ex kargs replace
vm_rpmostree ex kargs --append=REPLACE_TEST=TEST --append=REPLACE_MULTI_TEST=TEST --append=REPLACE_MULTI_TEST=NUMBERTWO

# test for replacing key/value pair with  only one value
vm_rpmostree ex kargs --replace=REPLACE_TEST=HELLO
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_file_has_content_literal tmp_conf.txt 'REPLACE_TEST=HELLO'
echo "ok replacing one key/value pair"

# test for replacing key/value pair with multi vars
if vm_rpmostree ex kargs --replace=REPLACE_MULTI_TEST=ERR 2>err.txt; then
    assert_not_reached "Replace a key with multiple values unexpectedly succeeded"
fi
assert_file_has_content err.txt "Unable to replace argument 'REPLACE_MULTI_TEST' with multiple values"
echo "ok failed to replace key with multiple values"

# test for replacing  one of the values for multi value keys
vm_rpmostree ex kargs --replace=REPLACE_MULTI_TEST=TEST=NEWTEST
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=NEWTEST"
assert_not_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=TEST"
assert_file_has_content tmp_conf.txt "REPLACE_MULTI_TEST=NUMBERTWO"
echo "ok replacing value from multi-valued key pairs"

# Note here: the conf file won't be exactly the same as the one we saved
# at the beginning of the test. The reason for that is when a deployment
# is being made, a new boot folder with different version in /ostree
# might be created , making the ostree term in the options no longer
# same, unable to find a way to avoid this yet.
vm_rpmostree rollback
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'REPLACE_MULTI_TEST=NUMBERTWO'
assert_not_file_has_content tmp_conf.txt 'REPLACE_MULTI_TEST=NEWTEST'
assert_not_file_has_content tmp_conf.txt 'APPENDARG=2NDAPPEND'
echo "ok rollback will revert the changes to conf file"
