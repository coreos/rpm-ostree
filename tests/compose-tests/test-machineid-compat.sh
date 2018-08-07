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

prepare_compose_test "machineid-compat"
pysetjsonmember "machineid-compat" 'False'
runcompose
echo "ok compose"

ostree --repo=${repobuild} ls ${treeref} /usr/etc > ls.txt
assert_not_file_has_content ls.txt 'machine-id'
echo "ok machineid-compat"
