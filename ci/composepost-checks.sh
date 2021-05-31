#!/bin/bash
# This script expects a coreos-assembler working directory
# and will validate parts of the generated ostree commit.

set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
commondir=$(cd "$dn/../tests/common" && pwd)
. "${commondir}/libtest-core.sh"

repo=tmp/repo
ref=$(cosa meta --get-value ref)

# Nothing in /boot (but it should exist)
ostree --repo="${repo}" ls -R "${ref}" /boot > tmp/bootls.txt
cat >tmp/bootls-expected.txt <<EOF
d00755 0 0      0 /boot
EOF
diff -u tmp/bootls{-expected,}.txt
# Verify /usr/lib/ostree-boot
ostree --repo="${repo}" ls -R "${ref}" /usr/lib/ostree-boot > tmp/bootls.txt
assert_not_file_has_content tmp/bootls.txt vmlinuz-
assert_not_file_has_content tmp/bootls.txt initramfs-
# And use the kver to find the kernel in /usr/lib/modules
ostree --repo="${repo}" ls -R "${ref}" /usr/lib/modules > tmp/modules-lsr.txt
assert_file_has_content tmp/modules-lsr.txt '/vmlinuz$'
assert_file_has_content tmp/modules-lsr.txt '/initramfs.img$'
echo "ok boot location modules"

ostree --repo="${repo}" show --print-metadata-key=ostree.bootable "${ref}" >out.txt
assert_file_has_content_literal out.txt 'true'
echo "ok bootable metadata"
