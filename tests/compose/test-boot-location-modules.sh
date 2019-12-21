#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set boot-location '"modules"'
runcompose
echo "ok compose"

# Nothing in /boot (but it should exist)
ostree --repo="${repo}" ls -R "${treeref}" /boot > bootls.txt
cat >bootls-expected.txt <<EOF
d00755 0 0      0 /boot
EOF
diff -u bootls{-expected,}.txt
# Verify /usr/lib/ostree-boot
ostree --repo="${repo}" ls -R "${treeref}" /usr/lib/ostree-boot > bootls.txt
assert_not_file_has_content bootls.txt vmlinuz-
assert_not_file_has_content bootls.txt initramfs-
# And use the kver to find the kernel in /usr/lib/modules
ostree --repo="${repo}" ls -R "${treeref}" /usr/lib/modules > modules-lsr.txt
assert_file_has_content modules-lsr.txt '/vmlinuz$'
assert_file_has_content modules-lsr.txt '/initramfs.img$'
echo "ok boot location modules"
