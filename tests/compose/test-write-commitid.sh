#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

runcompose --write-commitid-to $(pwd)/commitid.txt
wc -c < commitid.txt > wc.txt
assert_file_has_content_literal wc.txt 64
echo "ok compose"

# --write-commitid-to should not set the ref
ostree --repo=${repo} refs > refs.txt
assert_file_empty refs.txt
echo "ok ref not written"

commitid_txt=$(cat commitid.txt)
assert_streq "$(jq -r '.["ostree-commit"]' < compose.json)" "${commitid_txt}"
# And verify we have other keys
for key in ostree-version rpm-ostree-inputhash ostree-content-bytes-written; do
    jq -r '.["'${key}'"]' compose.json >/dev/null
done
echo "ok composejson"
