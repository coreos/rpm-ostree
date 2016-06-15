#!/bin/bash
#
# Copyright (C) 2015 Red Hat Inc.
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

check_root_test

# Let's create a temporary pkglibdir and tell rpm-ostree to use that dir
# instead of the (non-existent) installed version
mkdir pkglibdir
ln -s ${topsrcdir}/src/app/rpm-ostree-0-integration.conf pkglibdir/
export RPMOSTREE_UNINSTALLED_PKGLIBDIR=$PWD/pkglibdir

composedir=${commondir}/compose

arch=$(arch)
if ! test "${arch}" = x86_64; then
    echo 1>&2 "$0 can be run only on x86_64"; echo "1..0" ; exit 77
fi

testref=fedora/${arch}/test

echo "1..4"

ostree init --repo=repo --mode=archive-z2

echo "ok setup"

rpm-ostree --repo=repo compose --dry-run tree ${composedir}/test-repo.json
ostree --repo=repo refs >refs.txt
assert_file_empty refs.txt
rm refs.txt

echo "ok dry run"

rpm-ostree --repo=repo compose tree ${composedir}/test-repo.json
ostree --repo=repo refs >refs.txt
assert_file_has_content refs.txt ${testref}

echo "ok compose"

# bring them in the current context so we can modify exported_file
ln -s ${composedir}/test-repo-add-files.json .
ln -s ${composedir}/test-repo.repo .

echo hello > exported_file

rpm-ostree --repo=repo compose tree --touch-if-changed=$(pwd)/touched test-repo-add-files.json
assert_has_file touched
old_mtime=$(stat -c %y touched)
ostree --repo=repo ls fedora/test /exports/exported_file | grep exported > exported.txt

assert_file_has_content exported.txt "/exports/exported_file"
assert_file_has_content exported.txt "0 0"
ostree --repo=repo rev-parse fedora/test > oldref.txt
rpm-ostree --repo=repo compose tree --touch-if-changed=$(pwd)/touched test-repo-add-files.json
new_mtime=$(stat -c %y touched)
ostree --repo=repo rev-parse fedora/test > newref.txt
assert_streq $(cat oldref.txt) $(cat newref.txt)
assert_streq "$old_mtime" "$new_mtime"

echo . >> exported_file
rpm-ostree --repo=repo compose tree --touch-if-changed=$(pwd)/touched test-repo-add-files.json
new_mtime=$(stat -c %y touched)
ostree --repo=repo rev-parse fedora/test > newref.txt
assert_not_streq $(cat oldref.txt) $(cat newref.txt)
assert_not_streq "$old_mtime" "$new_mtime"

echo "ok compose add files"
