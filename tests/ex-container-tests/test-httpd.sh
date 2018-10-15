#!/usr/bin/bash
set -xeuo pipefail

cd ${test_tmpdir}

dn=$(cd $(dirname $0) && pwd)
. ${dn}/../common/libtest-core.sh

cat >httpd.conf <<EOF
[tree]
ref=httpd
packages=httpd;
selinux=false
repos=fedora;
releasever=28
EOF

# This one has non-root ownership in some of the dependencies, but we shouldn't
# try to apply them; see apply_rpmfi_overrides().
rpm-ostree ex container assemble httpd.conf
ostree --repo=repo ls httpd /usr/sbin/httpd
