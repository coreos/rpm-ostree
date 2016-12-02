#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_run_compose "basic"
ostree --repo=${repobuild} ls -R ${treeref} /usr/lib/ostree-boot > bootls.txt
assert_file_has_content bootls.txt vmlinuz
assert_file_has_content bootls.txt initramfs

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
