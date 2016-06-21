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

# try to do an upgrade -- there shouldn't be any
vm_cmd rpm-ostree upgrade > out
assert_file_has_content out 'No upgrade available\.'
echo "ok no upgrade available"

if vm_cmd rpm-ostree upgrade --check; then
  assert_not_reached "upgrade --check unexpectedly passed"
elif [ $? -ne 77 ]; then
  assert_not_reached "upgrade --check didn't exit 77"
fi
echo "ok upgrade --check"
