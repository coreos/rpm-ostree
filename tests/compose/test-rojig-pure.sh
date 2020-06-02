#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set "automatic-version-prefix" '"42"'
treefile_set "documentation" 'True'
mkdir rojig-repo
runcompose() {
    (cd rojig-repo && createrepo_c .) && \
    rm -f treecompose.json && \
    runasroot rpm-ostree compose rojig --write-composejson-to $(pwd)/treecompose.json --cachedir=$(pwd)/cache ${treefile} $(pwd)/rojig-repo "$@" && \
    (cd rojig-repo && createrepo_c .)
}

runcompose
test -f treecompose.json
test -f rojig-repo/x86_64/fedora-coreos-42-1.fc*.x86_64.rpm
echo "ok rojig ♲📦 initial"

runcompose
test '!' -f treecompose.json
echo "ok rojig no changes"

treefile_set "documentation" 'False'
runcompose
test -f treecompose.json
test -f rojig-repo/x86_64/fedora-coreos-42.1-1.fc*.x86_64.rpm
echo "ok rojig dropped docs"
