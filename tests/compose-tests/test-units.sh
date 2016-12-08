#!/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "units"
pysetjsonmember "default_target" '"multi-user.target"'
pyappendjsonmember "packages" '["tuned"]'
pysetjsonmember "units" '["tuned.service"]'
cat $treefile
runcompose
echo "ok compose"

ostree --repo=${repobuild} ls ${treeref} \
    /usr/etc/systemd/system/default.target > out.txt
assert_file_has_content out.txt '-> .*/multi-user\.target'
echo "ok default target"

ostree --repo=${repobuild} ls ${treeref} \
    /usr/etc/systemd/system/multi-user.target.wants > out.txt
assert_file_has_content out.txt '-> .*/tuned.service'
echo "ok enable units"
