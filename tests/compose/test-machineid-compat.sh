#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Test that `units` and `machineid-compat: False` conflict
treefile_set "units" '["tuned.service"]'

# Do the compose
if runcompose |& tee err.txt; then
    assert_not_reached err.txt "Successfully composed with units and machineid-compat=False?"
fi
assert_file_has_content_literal err.txt \
    "'units' directive is incompatible with machineid-compat = false"
echo "ok conflict with units"

# Now test machineid-compat: True

# Also test having no ref (XXX: move to misc or something)
treefile_del 'ref'
treefile_set "machineid-compat" 'True'
runcompose
echo "ok compose"

ostree --repo="${repo}" refs > refs.txt
assert_not_file_has_content refs.txt "${treeref}"
echo "ok no refs written"

commit=$(jq -r '.["ostree-commit"]' < compose.json)
ostree --repo=${repo} ls ${commit} /usr/etc > ls.txt
assert_file_has_content ls.txt 'machine-id'
echo "ok machineid-compat"
