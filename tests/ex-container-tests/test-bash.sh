#!/usr/bin/bash
set -xeuo pipefail

cd ${test_tmpdir}

cat >bash.conf <<EOF
[tree]
ref=bash
packages=coreutils;bash;
repos=fedora;
EOF

rpm-ostree ex container assemble bash.conf
