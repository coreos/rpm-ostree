#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

# specifying the key but neither automatic_version_prefix nor
# --add-metadata-string should cause no mutation

prepare_compose_test "mutate-os-release-none"
pysetjsonmember "mutate-os-release" '"26"'
runcompose
echo "ok compose (none)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-fedora > os-release.prop

assert_file_has_content os-release.prop VERSION_ID=26
assert_not_file_has_content os-release.prop OSTREE_VERSION=
assert_file_has_content os-release.prop 'VERSION="26 (Twenty Six)"'
echo "ok mutate-os-release-none"

# make sure --add-metadata-string has precedence and works with
# mutate-os-release

prepare_compose_test "mutate-os-release-cli"
pysetjsonmember "automatic_version_prefix" '"26.555"'
pysetjsonmember "mutate-os-release" '"26"'
runcompose --add-metadata-string=version=26.444
echo "ok compose (cli)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-fedora > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=26
assert_file_has_content os-release.prop OSTREE_VERSION=26.444
assert_file_has_content os-release.prop 'VERSION="26\.444 (Twenty Six)"'
echo "ok mutate-os-release-cli"

# make sure automatic_version_prefix works

prepare_compose_test "mutate-os-release-auto"
pysetjsonmember "automatic_version_prefix" '"26.555"'
pysetjsonmember "mutate-os-release" '"26"'
runcompose
echo "ok compose (auto)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-fedora > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=26
assert_file_has_content os-release.prop OSTREE_VERSION=26.555
assert_file_has_content os-release.prop 'VERSION="26\.555 (Twenty Six)"'
echo "ok mutate-os-release (auto)"
