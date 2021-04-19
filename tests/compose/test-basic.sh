#!/bin/bash
set -xeuo pipefail

# XXX: nuke this test once we fully drop non-unified core mode

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

# drop the --unified-core and add --workdir
mkdir -p cache/workdir
export compose_base_argv="${compose_base_argv/--unified-core/--workdir=$PWD/cache/workdir}"

# Test --parent at the same time (hash is `echo | sha256sum`)
rm -rf cache/workdir && mkdir cache/workdir
runcompose --add-metadata-from-json metadata.json \
  --parent=01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b

# shellcheck source=libbasic-test.sh
. "${dn}/libbasic-test.sh"
basic_test

# Check tmpfiles.d entries created by postprocessing /var.
# Testcases picked at random to cover translation logic. They are unlikely to change upstream,
# but if that happens we will need to adapt the entries.
ostree --repo="${repo}" cat "${treeref}" /usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf > autovar.txt
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 root root - -'
assert_file_has_content_literal autovar.txt 'd /var/log/chrony 0755 chrony chrony - -'
assert_file_has_content_literal autovar.txt 'L /var/mail - - - - spool/mail'
assert_file_has_content_literal autovar.txt 'd /var/tmp 1777 root root - -'
assert_file_has_content_literal autovar.txt 'd /var/lib/polkit-1 0750 root polkitd - -'
echo "ok autovar"

# Validate this exists
ostree --repo="${repo}" ls "${treeref}" /usr/lib/systemd/system/multi-user.target.wants/ostree-remount.service

python3 <<EOF
import json, yaml
tf=yaml.safe_load(open("$treefile"))
with open("$treefile.json", "w") as f:
  json.dump(tf, f)
EOF
export treefile=$treefile.json
rm -rf cache/workdir && mkdir cache/workdir
runcompose
echo "ok json"

# also check that --no-parent doesn't invalidate change detection
rm -rf cache/workdir && mkdir cache/workdir
runcompose --no-parent |& tee out.txt
assert_file_has_content_literal out.txt "No apparent changes since previous commit"
echo "ok --no-parent"
