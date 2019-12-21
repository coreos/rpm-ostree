#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

releasever=31

# make sure we clear out postprocess scripts, which might be using os-release
treefile_set "postprocess" '[]'

# specifying the key but neither automatic_version_prefix nor
# --add-metadata-string should cause no mutation
treefile_set "mutate-os-release" '"'${releasever}'"'
treefile_del 'automatic-version-prefix'
runcompose
echo "ok compose (none)"

ostree --repo=${repo} cat ${treeref} \
    /usr/lib/os-release > os-release.prop

assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_not_file_has_content os-release.prop OSTREE_VERSION=
assert_file_has_content os-release.prop 'VERSION="'${releasever}' (CoreOS'
echo "ok mutate-os-release-none"

# make sure --add-metadata-string has precedence and works with
# mutate-os-release

treefile_set "automatic-version-prefix" '"'${releasever}'.555"'
treefile_set "mutate-os-release" '"'${releasever}'"'
runcompose --add-metadata-string=version=${releasever}.444
echo "ok compose (cli)"

ostree --repo=${repo} cat ${treeref} \
    /usr/lib/os-release > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_file_has_content os-release.prop OSTREE_VERSION=\'${releasever}.444\'
assert_file_has_content os-release.prop 'VERSION="'${releasever}'\.444 (CoreOS'
echo "ok mutate-os-release-cli"

# make sure automatic_version_prefix works

treefile_set "automatic-version-prefix" '"'${releasever}'.555"'
treefile_set "mutate-os-release" '"'${releasever}'"'
runcompose --force-nocache
echo "ok compose (auto)"

ostree --repo=${repo} cat ${treeref} \
    /usr/lib/os-release > os-release.prop

# VERSION_ID *shouldn't* change
# (https://github.com/projectatomic/rpm-ostree/pull/433)
assert_file_has_content os-release.prop VERSION_ID=${releasever}
assert_file_has_content os-release.prop OSTREE_VERSION=\'${releasever}.555\'
assert_file_has_content os-release.prop 'VERSION="'${releasever}'\.555 (CoreOS'
echo "ok mutate-os-release (auto)"
