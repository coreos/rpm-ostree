#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo for recommends testing
treefile_append "repos" '["test-repo"]'
build_rpm foodep
build_rpm foobar recommends foobar-rec requires foodep
build_rpm foobar-rec

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
# the top-level manifest doesn't have any packages, so just set it
treefile_append "packages" '["foobar"]'
treefile_set 'recommends' "True"

runcompose --dry-run >log.txt
assert_file_has_content_literal log.txt 'foobar-1.0'
assert_file_has_content_literal log.txt 'foobar-rec-1.0'
rm -f log.txt
echo "ok no exclude"

# Test exclude
treefile_append "exclude-packages" '["foobar-rec"]'

runcompose --dry-run >log.txt
assert_file_has_content_literal log.txt 'foobar-1.0'
assert_not_file_has_content_literal log.txt 'foobar-rec-1.0'
rm -f log.txt
echo "ok exclude recommend"

treefile_append "exclude-packages" '["foodep"]'

if runcompose --dry-run &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt 'package foodep.*is filtered out by exclude filtering'
echo "ok exclude included"
