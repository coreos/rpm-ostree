#!/bin/bash
# Test rpm-ostree compose tree --ex-jigdo-output-rpm

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh
. ${dn}/../common/libtest.sh

prepare_compose_test "compose2jigdo"
pysetjsonmember "ex-jigdo-spec" '"fedora-atomic-host-oirpm.spec"'
mkdir cache
mkdir jigdo-output
runcompose --ex-jigdo-output-rpm $(pwd)/jigdo-output --cachedir $(pwd)/cache --add-metadata-string version=42.0
rev=$(ostree --repo=repo-build rev-parse ${treeref})
find jigdo-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'fedora-atomic-host-42.0.*x86_64'
grep 'fedora-atomic-host.*x86_64\.rpm' rpms.txt | while read p; do
    rpm -qp --provides ${p} >>provides.txt
done
assert_file_has_content_literal provides.txt "rpmostree-jigdo-commit(${rev})"
echo "ok compose2jigdo"
