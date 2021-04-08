#!/bin/bash
#
# NOTE: This is presently disabled by default because
# we don't want to drag pygobject3 into our build container
# and anyways the shared library should be considered deprecated.
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
echo "1..2"

set -x

if ! skip_one_with_asan; then
    cat >test-rpmostree-gi-arch <<EOF
#!/usr/bin/python3
import gi
gi.require_version("RpmOstree", "1.0")
from gi.repository import RpmOstree
assert RpmOstree.get_basearch() == 'x86_64'
assert RpmOstree.varsubst_basearch('http://example.com/foo/\${basearch}/bar') == 'http://example.com/foo/x86_64/bar'
EOF
    chmod a+x test-rpmostree-gi-arch
    case $(arch) in
        x86_64) ./test-rpmostree-gi-arch
                echo "ok rpmostree arch"
                ;;
        *) echo "ok # SKIP Skipping RPM architecture test on $(arch)"
    esac
fi

if ! skip_one_with_asan; then
    cat >test-rpmostree-gi <<EOF
#!/usr/bin/python3
import gi
gi.require_version("RpmOstree", "1.0")
from gi.repository import RpmOstree
assert RpmOstree.check_version(2017, 6)
# If this fails for you, please come back in a time machine and say hi
assert not RpmOstree.check_version(3000, 1)
EOF
    chmod a+x test-rpmostree-gi
    ./test-rpmostree-gi
    echo "ok rpmostree version"
fi
