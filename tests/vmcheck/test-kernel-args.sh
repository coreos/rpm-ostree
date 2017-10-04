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
assert_file_has_content_literal tmp_conf.txt 'FOO=BAR'
assert_file_has_content_literal tmp_conf.txt 'APPENDARG=VALAPPEND APPENDARG=2NDAPPEND'
echo "ok kargs append"

# test for rpm-ostree ex kargs delete
vm_rpmostree ex kargs --delete FOO
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_not_file_has_content tmp_conf.txt 'FOO=BAR'
echo "ok delete a single key/value pair"

if vm_rpmostree ex kargs --delete APPENDARG 2>err.txt; then
    assert_not_reached "Delete A key with multiple values unexpected succeeded"
fi
assert_file_has_content "Unable to delete APPENDARG with multiple values associated with it"
echo "ok failed to delete key with multiple values"

vm_rpmostree ex kargs --delete APPENDARG=VALAPPEND
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_not_file_has_content 'APPENDARG=VALAPPEND'
assert_file_has_content 'APPENDARG=2NDAPPEND'
echo "ok delete a single key/value pair"

# prove that changing kargs is a deployment and rollbackable
vm_rpmostree rollback
vm_cmd grep ^options /boot/loader/entries/ostree-$osname-0.conf > tmp_conf.txt
assert_file_has_content_literal tmp_conf.txt "$conf_content"
echo "ok rollback will revert the changes to conf file"
