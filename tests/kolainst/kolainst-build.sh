#!/bin/bash
# This pre-generates RPMs for testing that will be provided
# to the kola tests as data/, so we don't need to rpmbuild.
set -euo pipefail
dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(git rev-parse --show-toplevel)
commondir=$(cd "$dn/../common" && pwd)
export topsrcdir commondir
. "${commondir}/libtest.sh"

rm rpm-repos -rf
mkdir rpm-repos

test_tmpdir=$(mktemp -d)
repover=0

# Right now we build just one rpm, with one repo version,
# but the idea is to extend this with more.
mkdir rpm-repos/${repover}
build_rpm foo version 1.2 release 3
mv ${test_tmpdir}/yumrepo/* rpm-repos/${repover}
