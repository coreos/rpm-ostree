#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

echo "1..2"

rpm-ostree container init

cp ${commondir}/compose/test-repo.repo rpmmd.repos.d

cat >empty.conf <<EOF
[tree]
ref=empty
packages=empty
repos=test-repo
EOF

rpm-ostree container assemble empty.conf
assert_has_dir roots/empty.0
ostree --repo=repo rev-parse empty
echo "ok assemble"

cat >nobranch.conf <<EOF
[tree]
packages=empty
repos=test-repo
EOF
if rpm-ostree container assemble nobranch.conf 2>err.txt; then
    assert_not_reached "nobranch.conf"
fi

cat >nopackages.conf <<EOF
[tree]
ref=empty
packages=
repos=test-repo
EOF
if rpm-ostree container assemble nopackages.conf 2>err.txt; then
    assert_not_reached "nopackages.conf"
fi

cat >norepos.conf <<EOF
[tree]
ref=empty
packages=empty
EOF
if rpm-ostree container assemble norepos.conf 2>err.txt; then
    assert_not_reached "norepos.conf"
fi

cat >notfoundpackage.conf <<EOF
[tree]
ref=notfound
packages=notfound
repos=test-repo
EOF
if rpm-ostree container assemble notfound.conf 2>err.txt; then
    assert_not_reached "notfound.conf"
fi

echo "ok error conditions"
