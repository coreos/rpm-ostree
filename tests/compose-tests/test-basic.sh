#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "basic"
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
runcompose --add-metadata-from-json metadata.json

. ${dn}/libbasic-test.sh
basic_test

# This one is done by postprocessing /var
ostree --repo=${repobuild} cat ${treeref} /usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf > autovar.txt
# Picked this one at random as an example of something that won't likely be
# converted to tmpfiles.d upstream.  But if it is, we can change this test.
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 root root - -'
# And this one has a non-root uid
assert_file_has_content_literal autovar.txt 'd /var/log/chrony 0755 chrony chrony - -'
echo "ok autovar"

ostree --repo=${repobuild} cat ${treeref} /usr/lib/systemd/system-preset/40-rpm-ostree-auto.preset > preset.txt
assert_file_has_content preset.txt '^enable ostree-remount.service$'
assert_file_has_content preset.txt '^enable ostree-finalize-staged.path$'

prepare_compose_test "from-yaml"
python3 <<EOF
import json, yaml
jd=json.load(open("$treefile"))
with open("$treefile.yaml", "w") as f:
  yaml.safe_dump(jd, f)
EOF
export treefile=$treefile.yaml
runcompose
echo "ok yaml"

# also check that --no-parent doesn't invalidate change detection
runcompose --no-parent |& tee out.txt
assert_file_has_content_literal out.txt "No apparent changes since previous commit"
echo "ok --no-parent"
