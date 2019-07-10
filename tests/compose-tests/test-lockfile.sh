#!/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "lockfile"
# Add a local rpm-md repo so we can mutate local test packages
pyappendjsonmember "repos" '["test-repo"]'
build_rpm test-pkg-common
build_rpm test-pkg requires test-pkg-common
build_rpm another-test-pkg
# The test suite writes to pwd, but we need repos in composedata
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln yumrepo.repo composedata/test-repo.repo
pyappendjsonmember "packages" '["test-pkg", "another-test-pkg"]'
pysetjsonmember "documentation" 'False'
mkdir cache
# Create lockfile
runcompose --ex-write-lockfile-to=$PWD/versions.lock --cachedir $(pwd)/cache
rpm-ostree --repo=${repobuild} db list ${treeref} > test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-1.0-1.x86_64'
echo "ok compose"

assert_has_file "versions.lock"
assert_jq versions.lock \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg"].evra = "1.0-1.x86_64"'
echo "ok lockfile created"
# Read lockfile back
build_rpm test-pkg-common version 2.0
build_rpm test-pkg version 2.0 requires test-pkg-common
build_rpm another-test-pkg version 2.0
runcompose --ex-lockfile=$PWD/versions.lock --cachedir $(pwd)/cache
echo "ok compose with lockfile"

rpm-ostree --repo=${repobuild} db list ${treeref} > test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-1.0-1.x86_64'
echo "ok lockfile read"

# now add an override and check that not specifying a digest is allowed
cat > override.lock <<EOF
{
  "packages": {
    "another-test-pkg": {
      "evra": "2.0-1.x86_64"
    }
  }
}
EOF

runcompose --dry-run \
  --ex-lockfile=$PWD/versions.lock \
  --ex-lockfile=$PWD/override.lock \
  --ex-write-lockfile-to=$PWD/versions.lock \
  --cachedir $(pwd)/cache |& tee out.txt
echo "ok compose with lockfile"

assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-2.0-1.x86_64'
assert_jq versions.lock \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg"].evra = "2.0-1.x86_64"'
echo "ok override"
