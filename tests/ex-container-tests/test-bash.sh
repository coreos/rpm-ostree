#!/usr/bin/bash
set -xeuo pipefail

cd ${test_tmpdir}

dn=$(cd $(dirname $0) && pwd)
. ${dn}/../common/libtest-core.sh

cat >bash.conf <<EOF
[tree]
ref=bash
packages=coreutils;bash;
repos=fedora;
EOF

rpm-ostree ex container assemble bash.conf
ostree --repo=repo fsck -q
ostree --repo=repo ls bash /usr/etc/shadow > shadowls.txt
assert_file_has_content shadowls.txt '^-00400 .*/usr/etc/shadow'
