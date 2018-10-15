#!/usr/bin/bash
set -xeuo pipefail

cd ${test_tmpdir}

dn=$(cd $(dirname $0) && pwd)
. ${dn}/../common/libtest-core.sh

cat >bash.conf <<EOF
[tree]
ref=bash
packages=coreutils;bash;
selinux=false
repos=fedora;
releasever=28
EOF

rpm-ostree ex container assemble bash.conf
ostree --repo=repo fsck -q
ostree --repo=repo ls bash /usr/etc/shadow > shadowls.txt
assert_file_has_content shadowls.txt '^-00400 .*/usr/etc/shadow'
ostree --repo=repo ls bash /usr/share/doc/bash/README >/dev/null

cat >bash-nodocs.conf <<EOF
[tree]
ref=bash-nodocs
packages=coreutils;bash;
selinux=false
repos=fedora;
releasever=28
documentation=false;
EOF

rpm-ostree ex container assemble bash-nodocs.conf
ostree --repo=repo ls bash-nodocs /usr/share/doc/bash >docs.txt
assert_not_file_has_content docs.txt README
