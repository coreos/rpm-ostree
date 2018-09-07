#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "basic-unified"
# Test metadata json with objects, arrays, numbers
cat > metadata.json <<EOF
{
  "exampleos.gitrepo": {
     "rev": "97ec21c614689e533d294cdae464df607b526ab9",
     "src": "https://gitlab.com/exampleos/custom-atomic-host"
  },
  "exampleos.tests": ["smoketested", "e2e"]
}
EOF
runcompose --ex-unified-core --add-metadata-from-json metadata.json

. ${dn}/libbasic-test.sh
basic_test

# This one is done by postprocessing /var
ostree --repo=${repobuild} cat ${treeref} /usr/lib/tmpfiles.d/pkg-filesystem.conf > autovar.txt
# Picked this one at random as an example of something that won't likely be
# converted to tmpfiles.d upstream.  But if it is, we can change this test.
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 root root - -'
ostree --repo=${repobuild} cat ${treeref} /usr/lib/tmpfiles.d/pkg-chrony.conf > autovar.txt
# And this one has a non-root uid
assert_file_has_content_literal autovar.txt 'd /var/log/chrony 0755 chrony chrony - -'
# see rpmostree-importer.c
if ostree --repo=${repobuild} cat ${treeref} /usr/lib/tmpfiles.d/pkg-rpm.conf > rpm.txt 2>/dev/null; then
    assert_not_file_has_content rpm.txt 'd /var/lib/rpm'
fi
echo "ok autovar"

# And redo it to trigger relabeling
origrev=$(ostree --repo=${repobuild} rev-parse ${treeref})
runcompose  --force-nocache --ex-unified-core
newrev=$(ostree --repo=${repobuild} rev-parse ${treeref})
assert_not_streq "${origrev}" "${newrev}"

echo "ok rerun"
