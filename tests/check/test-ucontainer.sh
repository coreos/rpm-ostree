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

rpm-ostree ex container init
if test -n "${OSTREE_NO_XATTRS:-}"; then
    echo -e 'disable-xattrs=true\n' >> repo/config
fi

build_rpm foo
build_rpm fake-shell provides /usr/bin/sh

cat > rpmmd.repos.d/test-repo.repo <<EOF
[test-repo]
baseurl=file://$PWD/yumrepo
gpgcheck=0
EOF

cat > foo.conf <<EOF
[tree]
ref=foo
packages=foo
repos=test-repo
skip-sanity-check=true
EOF

rpm-ostree ex container assemble foo.conf
assert_has_dir roots/foo.0
ostree --repo=repo rev-parse foo
echo "ok assemble"

cat >nobranch.conf <<EOF
[tree]
packages=foo
repos=test-repo
EOF
if rpm-ostree ex container assemble nobranch.conf 2>err.txt; then
    assert_not_reached "nobranch.conf"
fi

cat >nopackages.conf <<EOF
[tree]
ref=foo
packages=
repos=test-repo
EOF
if rpm-ostree ex container assemble nopackages.conf 2>err.txt; then
    assert_not_reached "nopackages.conf"
fi

cat >norepos.conf <<EOF
[tree]
ref=foo
packages=foo
EOF
if rpm-ostree ex container assemble norepos.conf 2>err.txt; then
    assert_not_reached "norepos.conf"
fi

cat >notfoundpackage.conf <<EOF
[tree]
ref=notfound
packages=notfound
repos=test-repo
EOF
if rpm-ostree ex container assemble notfound.conf 2>err.txt; then
    assert_not_reached "notfound.conf"
fi

echo "ok error conditions"
