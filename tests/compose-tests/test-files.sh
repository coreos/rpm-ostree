#!/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "files"
pysetjsonmember "add-files" '[["foo.txt", "/usr/etc/foo.txt"]]'
pysetjsonmember "remove-files" '["etc/hosts"]'
pysetjsonmember "remove-from-packages" '[["setup", "/etc/hosts\..*"]]'
rnd=$RANDOM
echo $rnd > composedata/foo.txt
runcompose
echo "ok compose"

ostree --repo=${repobuild} cat ${treeref} /usr/etc/foo.txt > out.txt
assert_file_has_content out.txt $rnd
echo "ok add-files"

ostree --repo=${repobuild} ls ${treeref} /usr/etc > out.txt
assert_not_file_has_content out.txt '/usr/etc/hosts$'
echo "ok remove-files"

ostree --repo=${repobuild} ls ${treeref} /usr/etc > out.txt
assert_not_file_has_content out.txt '/usr/etc/hosts\.allow$'
assert_not_file_has_content out.txt '/usr/etc/hosts\.deny$'
echo "ok remove-from-packages"
