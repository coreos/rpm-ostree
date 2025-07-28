#!/bin/bash
set -euo pipefail

dn=$(cd $(dirname $0) && pwd)
commondir=${dn}/../common
. "$commondir/libtest.sh"

set -x

# We use TAP
echo 1..1

# Run a script in the host environment
td=$(mktemp -d)
cd $td
cat >script <<EOF
echo hello
echo someerr 1>&2
echo world
EOF
rpm-ostree internals bwrap-script / /bin/bash $(pwd)/script >out.txt
assert_file_has_content_literal out.txt 'script: hello
script: someerr
script: world'

echo "ok bwrap script"