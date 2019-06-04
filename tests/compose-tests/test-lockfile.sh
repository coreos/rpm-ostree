#!/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "lockfile"
# Add a local rpm-md repo so we can mutate local test packages
pyappendjsonmember "repos" '["test-repo"]'
build_rpm test-pkg \
          files "/usr/bin/test-pkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo localpkg data > %{buildroot}/usr/bin/test-pkg"
# The test suite writes to pwd, but we need repos in composedata
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln yumrepo.repo composedata/test-repo.repo
pyappendjsonmember "packages" '["test-pkg"]'
pysetjsonmember "documentation" 'False'
mkdir cache
# Create lockfile
runcompose --ex-write-lockfile-to=$PWD/versions.lock --cachedir $(pwd)/cache
npkgs=$(rpm-ostree --repo=${repobuild} db list ${treeref} |grep -v '^ostree commit' | wc -l)
echo "npkgs=${npkgs}"
rpm-ostree --repo=${repobuild} db list ${treeref} test-pkg >test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
echo "ok compose"

assert_has_file "versions.lock"
assert_file_has_content $PWD/versions.lock 'packages'
assert_file_has_content $PWD/versions.lock 'test-pkg-1.0-1.x86_64'
echo "lockfile created"
# Read lockfile back
build_rpm test-pkg \
          version 2.0 \
          files "/usr/bin/test-pkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo localpkg data > %{buildroot}/usr/bin/test-pkg"
runcompose --ex-lockfile=$PWD/versions.lock --cachedir $(pwd)/cache
echo "ok compose with lockfile"

rpm-ostree --repo=${repobuild} db list ${treeref} test-pkg >test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
echo "lockfile read"
