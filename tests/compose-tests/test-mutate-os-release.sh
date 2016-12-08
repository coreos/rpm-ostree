#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "mutate-os-release"
pysetjsonmember "automatic_version_prefix" '"25.555"'
pysetjsonmember "mutate-os-release" '"25"'
runcompose
echo "ok compose"

ostree --repo=${repobuild} cat ${treeref} \
    /usr/lib/os.release.d/os-release-fedora > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=25
assert_file_has_content os-release.prop OSTREE_VERSION=25.555
assert_file_has_content os-release.prop 'VERSION="25\.555 (Twenty Five)"'

echo "ok mutate-os-release"
