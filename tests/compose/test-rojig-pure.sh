#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh
. ${dn}/../common/libtest.sh

prepare_compose_test "rojig-pure"
pysetjsonmember "automatic_version_prefix" '"42"'
mkdir cache
mkdir rojig-repo
runcompose() {
    (cd rojig-repo && createrepo_c .) && \
    rm -f treecompose.json && \
    rpm-ostree compose rojig --write-composejson-to $(pwd)/treecompose.json --cachedir=$(pwd)/cache ${treefile} $(pwd)/rojig-repo "$@" && \
    (cd rojig-repo && createrepo_c .)
}

runcompose
test -f treecompose.json
test -f rojig-repo/x86_64/fedora-atomic-host-42-1.fc29.x86_64.rpm
echo "ok rojig ♲📦 initial"

runcompose
test '!' -f treecompose.json
echo "ok rojig no changes"

pysetjsonmember "documentation" 'False'
runcompose
test -f treecompose.json
test -f rojig-repo/x86_64/fedora-atomic-host-42.1-1.fc29.x86_64.rpm
echo "ok rojig dropped docs"
