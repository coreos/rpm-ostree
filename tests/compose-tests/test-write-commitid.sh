#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "write-commitid"
runcompose --write-commitid-to $(pwd)/commitid.txt
wc -c < commitid.txt > wc.txt
assert_file_has_content_literal wc.txt 64
echo "ok compose"

# --write-commitid-to should not set the ref
if ostree --repo=${repobuild} rev-parse ${treeref}; then
    fatal "Found ${treeref} ?"
fi
echo "ok ref not written"
