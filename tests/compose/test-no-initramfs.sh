#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set "no-initramfs" 'True'
runcompose
echo "ok compose"

commit=$(jq -r '.["ostree-commit"]' < compose.json)
ostree --repo=${repo:?} ls -R ${commit} usr/lib/modules > ls.txt
assert_not_file_has_content ls.txt '/usr/lib/modules/.*/initramfs.img$'
echo "ok no initramfs"
