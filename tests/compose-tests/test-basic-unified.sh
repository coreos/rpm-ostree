#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "basic-unified"
# Test --print-only, currently requires --repo.  We also
# just in this test (for now) use ${basearch} to test substitution.
pysetjsonmember "ref" '"fedora/stable/${basearch}/basic-unified"'
rpm-ostree compose tree --repo=${repobuild} --print-only ${treefile} > treefile.json
# Verify it's valid JSON
jq -r .ref < treefile.json > ref.txt
# Test substitution of ${basearch}
assert_file_has_content_literal ref.txt "${treeref}"
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

# Run it again, but without RPMOSTREE_PRESERVE_TMPDIR. Should be a no-op. This
# exercises fd handling in the tree context.
rpm-ostree compose tree ${compose_base_argv} ${treefile} "$@"
echo "ok no cachedir"

. ${dn}/libbasic-test.sh
basic_test

assert_file_has_content_literal compose-output.txt "Currently running in unified"
# And check that we *didn't* print a warning
assert_not_file_has_content_literal compose-output.txt \
  "warning: In the future, the default compose mode will be --unified-mode"
echo "ok current mode and legacy warning"

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

# And verify we're not hardlinking zero-sized files since this path isn't using
# rofiles-fuse
co=${repobuild}/tmp/usr-etc
ostree --repo=${repobuild} checkout -UHz --subpath=/usr/etc ${treeref} ${co}
# Verify the files exist and are zero-sized
for f in ${co}/sub{u,g}id; do
    test -f "$f"
    test '!' -s "$f"
done
if files_are_hardlinked ${co}/sub{u,g}id; then
    fatal "Hardlinked zero-sized files without cachedir"
fi
rm ${co} -rf
echo "ok no cachedir zero-sized hardlinks"

# And redo it to trigger relabeling
origrev=$(ostree --repo=${repobuild} rev-parse ${treeref})
runcompose  --force-nocache --ex-unified-core
newrev=$(ostree --repo=${repobuild} rev-parse ${treeref})
assert_not_streq "${origrev}" "${newrev}"

echo "ok rerun"
