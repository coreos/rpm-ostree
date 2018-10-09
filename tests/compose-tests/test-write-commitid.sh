#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "write-commitid"
treeref=""
runcompose --write-commitid-to $(pwd)/commitid.txt
wc -c < commitid.txt > wc.txt
assert_file_has_content_literal wc.txt 64
echo "ok compose"

# --write-commitid-to should not set the ref
if ostree --repo=${repobuild} rev-parse ${treeref}; then
    fatal "Found ${treeref} ?"
fi
echo "ok ref not written"

commitid_txt=$(cat commitid.txt)
assert_streq "${commit}" "${commitid_txt}"
# And verify we have other keys
for key in ostree-version rpm-ostree-inputhash ostree-content-bytes-written; do
    jq -r '.["'${key}'"]' ${composejson} >/dev/null
done

echo "ok composejson"
