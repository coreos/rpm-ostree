#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh
releasever=29

# specifying the key but neither automatic_version_prefix nor
# --add-metadata-string should cause no mutation

prepare_compose_test "mutate-os-release-none"
pysetjsonmember "mutate-os-release" '"'${releasever}'"'
runcompose
echo "ok compose (none)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-atomichost > os-release.prop

assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_not_file_has_content os-release.prop OSTREE_VERSION=
assert_file_has_content os-release.prop 'VERSION="'${releasever}' (Atomic '
echo "ok mutate-os-release-none"

# make sure --add-metadata-string has precedence and works with
# mutate-os-release

prepare_compose_test "mutate-os-release-cli"
pysetjsonmember "automatic_version_prefix" '"'${releasever}'.555"'
pysetjsonmember "mutate-os-release" '"'${releasever}'"'
runcompose --add-metadata-string=version=${releasever}.444
echo "ok compose (cli)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-atomichost > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_file_has_content os-release.prop OSTREE_VERSION=\'${releasever}.444\'
assert_file_has_content os-release.prop 'VERSION="'${releasever}'\.444 (Atomic '
echo "ok mutate-os-release-cli"

# make sure automatic_version_prefix works

prepare_compose_test "mutate-os-release-auto"
pysetjsonmember "automatic_version_prefix" '"'${releasever}'.555"'
pysetjsonmember "mutate-os-release" '"'${releasever}'"'
runcompose
echo "ok compose (auto)"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-atomichost > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_file_has_content os-release.prop OSTREE_VERSION=\'${releasever}.555\'
assert_file_has_content os-release.prop 'VERSION="'${releasever}'\.555 (Atomic '
echo "ok mutate-os-release (auto)"
