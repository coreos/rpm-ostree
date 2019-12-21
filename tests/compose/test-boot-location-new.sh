#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set boot-location '"new"'
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
assert_file_has_content bootls.txt vmlinuz-
assert_file_has_content bootls.txt initramfs-
kver=$(grep /vmlinuz bootls.txt | sed -e 's,.*/vmlinuz-\(.*\)-[0-9a-f].*$,\1,')
# And use the kver to find the kernel in /usr/lib/modules
ostree --repo="${repo}" ls "${treeref}" "/usr/lib/modules/${kver}"/{vmlinuz,initramfs.img} >/dev/null
echo "ok boot location new"
