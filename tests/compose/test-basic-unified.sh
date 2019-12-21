#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
# test `recommends: false` (test-misc-tweaks tests the true path)
build_rpm foobar recommends foobar-rec
build_rpm foobar-rec

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
treefile_append "packages" '["foobar"]'

# Test --print-only.  We also
# just in this test (for now) use ${basearch} to test substitution.
# shellcheck disable=SC2016
treefile_set_ref '"fedora/stable/${basearch}/basic-unified"'
rpm-ostree compose tree --print-only "${treefile}" > treefile.json

# Verify it's valid JSON
jq -r .ref < treefile.json > ref.txt
# Test substitution of ${basearch}
assert_file_has_content_literal ref.txt "${treeref}"

treefile_pyedit "tf['add-commit-metadata']['foobar'] = 'bazboo'"
treefile_pyedit "tf['add-commit-metadata']['overrideme'] = 'old var'"

# Test metadata json with objects, arrays, numbers
cat > metadata.json <<EOF
{
  "exampleos.gitrepo": {
     "rev": "97ec21c614689e533d294cdae464df607b526ab9",
     "src": "https://gitlab.com/exampleos/custom-atomic-host"
  },
  "exampleos.tests": ["smoketested", "e2e"],
  "overrideme": "new val"
}
EOF

# Test --parent at the same time (hash is `echo | sha256sum`)
runcompose --add-metadata-from-json $(pwd)/metadata.json \
  --parent 01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b

# Run it again, but without RPMOSTREE_PRESERVE_TMPDIR. Should be a no-op. This
# exercises fd handling in the tree context.
(unset RPMOSTREE_PRESERVE_TMPDIR && runcompose)
echo "ok no cachedir"

# shellcheck source=libbasic-test.sh
. "${dn}/libbasic-test.sh"
basic_test

# This one is done by postprocessing /var
ostree --repo="${repo}" cat "${treeref}" /usr/lib/tmpfiles.d/pkg-filesystem.conf > autovar.txt
# Picked this one at random as an example of something that won't likely be
# converted to tmpfiles.d upstream.  But if it is, we can change this test.
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 root root - -'
ostree --repo="${repo}" cat "${treeref}" /usr/lib/tmpfiles.d/pkg-chrony.conf > autovar.txt
# And this one has a non-root uid
assert_file_has_content_literal autovar.txt 'd /var/log/chrony 0755 chrony chrony - -'
# see rpmostree-importer.c
if ostree --repo="${repo}" cat "${treeref}" /usr/lib/tmpfiles.d/pkg-rpm.conf > rpm.txt 2>/dev/null; then
    assert_not_file_has_content rpm.txt 'd /var/lib/rpm'
fi
echo "ok autovar"

# And redo it to trigger relabeling. Also test --no-parent at the same time.
origrev=$(ostree --repo="${repo}" rev-parse "${treeref}")
runcompose --force-nocache --no-parent
newrev=$(ostree --repo="${repo}" rev-parse "${treeref}")
assert_not_streq "${origrev}" "${newrev}"
echo "ok rerun"

# And check that --no-parent worked.
if ostree rev-parse --repo "${repo}" "${newrev}"^ 2>error.txt; then
  assert_not_reached "New revision has a parent even with --no-parent?"
fi
assert_file_has_content_literal error.txt 'has no parent'
echo "ok --no-parent"
