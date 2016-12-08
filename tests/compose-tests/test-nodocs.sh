#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "nodocs"
pysetjsonmember "documentation" "False"
runcompose
echo "ok compose"

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_not_file_has_content manpages.txt man5/ostree.repo.5
echo "ok no manpages"
