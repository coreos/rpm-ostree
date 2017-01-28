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

selfdir=$(dirname $0)
. ${commondir}/libtest.sh

echo "1..4"

rpm-ostree container init

cp ${commondir}/compose/test-repo.repo rpmmd.repos.d

cat >empty.conf <<EOF
[tree]
ref=empty
packages=empty
repos=test-repo
EOF

rpm-ostree container assemble-checkout empty.conf
assert_has_dir roots/empty.0
test -f roots/empty/usr/etc/group
ostree --repo=repo rev-parse empty

cat >empty-docker.conf <<EOF
[tree]
ref=empty-docker
packages=empty
repos=test-repo
flavor=docker
EOF

rpm-ostree container assemble-checkout empty-docker.conf
assert_has_dir roots/empty-docker.0
ostree --repo=repo rev-parse empty-docker
test -f roots/empty-docker/etc/group
echo "ok assemble"

ostree --repo=repo refs --delete empty-docker
rpm-ostree container assemble-export empty-docker.conf
ostree --repo=repo rev-parse empty-docker
echo "ok assemble-export basic"

ostree --repo=repo refs --delete empty-docker
rpm-ostree container assemble-export --postprocess-from-host=${selfdir}/postprocess.sh empty-docker.conf
ostree --repo=repo checkout -U empty-docker empty-docker-co
assert_file_has_content empty-docker-co/foo foo
rm empty-docker-co -rf
echo "ok assemble-export postprocess"

cat >nobranch.conf <<EOF
[tree]
packages=empty
repos=test-repo
EOF
if rpm-ostree container assemble-checkout nobranch.conf 2>err.txt; then
    assert_not_reached "nobranch.conf"
fi

cat >nopackages.conf <<EOF
[tree]
ref=empty
packages=
repos=test-repo
EOF
if rpm-ostree container assemble-checkout nopackages.conf 2>err.txt; then
    assert_not_reached "nopackages.conf"
fi

cat >norepos.conf <<EOF
[tree]
ref=empty
packages=empty
EOF
if rpm-ostree container assemble-checkout norepos.conf 2>err.txt; then
    assert_not_reached "norepos.conf"
fi

cat >notfoundpackage.conf <<EOF
[tree]
ref=notfound
packages=notfound
repos=test-repo
EOF
if rpm-ostree container assemble-checkout notfound.conf 2>err.txt; then
    assert_not_reached "notfound.conf"
fi

echo "ok error conditions"
