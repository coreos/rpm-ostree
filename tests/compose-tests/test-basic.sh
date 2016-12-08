#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_run_compose "basic"
echo "ok compose"

ostree --repo=${repobuild} ls -R ${treeref} /usr/lib/ostree-boot > bootls.txt
assert_file_has_content bootls.txt vmlinuz
assert_file_has_content bootls.txt initramfs
echo "ok boot files"

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
echo "ok manpages"
