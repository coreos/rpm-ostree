#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
# test `recommends: false` (test-misc-tweaks tests the true path)
build_rpm foobar recommends foobar-rec post "test -f /run/ostree-booted"
build_rpm foobar-rec

uinfo_cmd add TEST-SEC-LOW security low
build_rpm vuln-pkg uinfo TEST-SEC-LOW
uinfo_cmd add-ref TEST-SEC-LOW 1 http://example.com/vuln1 "CVE-12-34 vuln1"

build_rpm testpkg-stdout-and-stderr \
             post "set -euo pipefail
echo testpkg-some-stdout-testing
echo testpkg-some-stderr-testing 1>&2
echo testpkg-more-stdout-testing
echo testpkg-more-stderr-testing 1>&2"

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
treefile_append "packages" '["vuln-pkg", "testpkg-stdout-and-stderr"]'

treefile_pyedit "
tf['repo-packages'] = [{
  'repo': 'test-repo',
  'packages': ['foobar'],
}]
"

treefile_set "postprocess" '["""#!/bin/bash
set -euo pipefail
echo postprocess-some-stdout-testing
echo postprocess-some-stderr-testing 1>&2
echo postprocess-more-stdout-testing
echo postprocess-more-stderr-testing 1>&2"""]'

# also test repovar substitution
treefile_pyedit "tf['repovars'] = {
  'foobar': 'yumrepo',
  'unused': 'bazboo',
}"
sed -i -e 's,baseurl=\(.*\)/yumrepo,baseurl=\1/$foobar,' yumrepo.repo
assert_file_has_content_literal yumrepo.repo '$foobar'

# Test --print-only.  We also
# just in this test (for now) use ${basearch} to test substitution.
# shellcheck disable=SC2016
treefile_set_ref '"fedora/stable/${basearch}/basic-unified"'
rpm-ostree compose tree --print-only "${treefile}" > treefile.json

# Verify it's valid JSON
jq -r .ref < treefile.json > ref.txt
# Test substitution of ${basearch}
assert_file_has_content_literal ref.txt "${treeref}"

treefile_pyedit "tf['base-refspec'] = 'somebaseref'"
rpm-ostree compose tree --print-only "${treefile}" > treefile.json
if runcompose --dry-run &>err.txt; then
  fatal "ran a compose with derivation"
fi
assert_file_has_content_literal err.txt 'the following derivation fields are not supported'
rm -f err.txt
treefile_pyedit "del tf['base-refspec']"
echo "ok cannot use derivation for composes yet"


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
  --parent 01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b |& tee compose.log

cat compose.log
assert_streq $(grep -cEe 'testpkg-(some|more)-(stdout|stderr)-testing' compose.log) 4
assert_streq $(grep -cEe 'postprocess-(some|more)-(stdout|stderr)-testing' compose.log) 4

# Run it again, but without RPMOSTREE_PRESERVE_TMPDIR. Should be a no-op. This
# exercises fd handling in the tree context.
(unset RPMOSTREE_PRESERVE_TMPDIR && runcompose)
echo "ok no cachedir"

# shellcheck source=libbasic-test.sh
. "${dn}/libbasic-test.sh"
basic_test

# This one is done by postprocessing /var
rpmostree_tmpfiles_path="/usr/lib/rpm-ostree/tmpfiles.d"
ostree --repo="${repo}" cat "${treeref}" ${rpmostree_tmpfiles_path}/filesystem.conf > autovar.txt
# Picked this one at random as an example of something that won't likely be
# converted to tmpfiles.d upstream.  But if it is, we can change this test.
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 root root - -'
ostree --repo="${repo}" cat "${treeref}" ${rpmostree_tmpfiles_path}/chrony.conf > autovar.txt
# And this one has a non-root uid
assert_file_has_content_literal autovar.txt 'd /var/lib/chrony 0750 chrony chrony - -'
# see rpmostree-importer.c
if ostree --repo="${repo}" cat "${treeref}" ${rpmostree_tmpfiles_path}/rpm.conf > rpm.txt 2>/dev/null; then
    assert_not_file_has_content rpm.txt 'd /var/lib/rpm'
fi

echo "ok autovar"

rpm-ostree db list --repo="${repo}" "${treeref}" --advisories > db-list-adv.txt
assert_file_has_content_literal db-list-adv.txt TEST-SEC-LOW

uinfo_cmd add TEST-SEC-CRIT security critical
build_rpm vuln-pkg version 2.0 uinfo TEST-SEC-CRIT
uinfo_cmd add-ref TEST-SEC-CRIT 2 http://example.com/vuln2 "CVE-56-78 vuln2"
echo "ok db list --advisories"

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

rpm-ostree db list --repo="${repo}" "${treeref}" --advisories > db-list-adv.txt
assert_not_file_has_content_literal db-list-adv.txt TEST-SEC-LOW
assert_file_has_content_literal db-list-adv.txt TEST-SEC-CRIT
rpm-ostree db diff --repo="${repo}" "${origrev}" "${newrev}" --advisories > db-diff-adv.txt
assert_not_file_has_content_literal db-diff-adv.txt TEST-SEC-LOW
assert_file_has_content_literal db-diff-adv.txt TEST-SEC-CRIT
echo "ok db diff --advisories"

build_rpm dodo-base
build_rpm dodo requires dodo-base
build_rpm solitaire

# this is pretty terrible... need --json for `rpm-ostree db list`
kernel_vra=$(rpm-ostree db list --repo=${repo} ${treeref} kernel | tail -n1 | cut -d- -f2-)
kernel_v=$(cut -d- -f1 <<< "$kernel_vra")
kernel_ra=$(cut -d- -f2- <<< "$kernel_vra")
kernel_r=${kernel_ra%.x86_64}

build_rpm kernel-core version ${kernel_v} release ${kernel_r}
build_rpm kernel-devel version ${kernel_v} release ${kernel_r}
build_rpm kernel-headers version ${kernel_v} release ${kernel_r}

cat > extensions.yaml << EOF
extensions:
  extinct-birds:
    packages:
      - dodo
      - solitaire
  another-arch:
    packages:
      - nonexistent
    architectures:
      - badarch
  kernel-devel:
    kind: development
    packages:
      - kernel-core
      - kernel-devel
      - kernel-headers
    match-base-evr: kernel
EOF

# we don't actually need root here, but in CI the cache may be in a qcow2 and
# the supermin code is gated behind `runasroot`
runasroot rpm-ostree compose extensions --repo=${repo} \
  --cachedir=${test_tmpdir}/cache --base-rev ${treeref} \
  --output-dir extensions ${treefile} extensions.yaml \
  --touch-if-changed extensions-changed

ls extensions/{dodo-1.0,dodo-base-1.0,solitaire-1.0}-*.rpm
ls extensions/kernel-{core,devel,headers}-${kernel_v}-${kernel_r}.x86_64.rpm
test -f extensions-changed
assert_jq extensions/extensions.json \
  '.extensions|length == 2' \
  '.extensions["extinct-birds"]' \
  '.extensions["kernel-devel"]'
echo "ok extensions"

rm extensions-changed
runasroot rpm-ostree compose extensions --repo=${repo} \
  --cachedir=${test_tmpdir}/cache \
  --output-dir extensions ${treefile} extensions.yaml \
  --touch-if-changed extensions-changed
if test -f extensions-changed; then
  fatal "found extensions-changed"
fi
echo "ok extensions no change"
