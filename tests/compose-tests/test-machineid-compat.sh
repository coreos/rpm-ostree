#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

# Test that `units` and `machineid-compat: False` conflict
prepare_compose_test "machineid-compat-conflict"
pysetjsonmember "machineid-compat" 'False'
pysetjsonmember "units" '["tuned.service"]'

# Do the compose -- we call compose directly because `set -e` has no effect when
# calling functions within an if condition context
if rpm-ostree compose tree ${compose_base_argv} ${treefile} |& tee err.txt; then
    assert_not_reached err.txt "Successfully composed with units and machineid-compat=False?"
fi
assert_file_has_content_literal err.txt \
    "'units' directive is incompatible with machineid-compat = false"
echo "ok conflict with units"

# In this test we also want to test that include:
# correctly handles machineid-compat.
prepare_compose_test "machineid-compat"
# Also test having no ref
pyeditjson 'del jd["ref"]' < ${treefile} > ${treefile}.new
mv ${treefile}{.new,}
treeref=""
pysetjsonmember "machineid-compat" 'False'
cat > composedata/fedora-machineid-compat-includer.yaml <<EOF
include: fedora-machineid-compat.json
EOF
export treefile=composedata/fedora-machineid-compat-includer.yaml
runcompose
echo "ok compose"

ostree --repo="${repobuild}" refs >refs.txt
diff -u /dev/null refs.txt
echo "ok no refs written"

ostree --repo=${repobuild} ls ${commit} /usr/etc > ls.txt
assert_not_file_has_content ls.txt 'machine-id'
echo "ok machineid-compat"
