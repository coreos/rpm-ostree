#!/bin/bash
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

origindir=/etc/rpm-ostree/origin.d
mkdir -p "${origindir}"
cat > "${origindir}/clienterror.yaml" << 'EOF'
base-refspec: "foo/x86_64/bar"
EOF
if rpm-ostree ex rebuild 2>err.txt; then
   fatal "did rebuild with base-refspec"
fi

# Test the no-op rebuild path
rm "${origindir}/clienterror.yaml"
rpm-ostree ex rebuild

echo ok
