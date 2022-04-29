#!/bin/bash
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

# Verify container flow
if rpm-ostree status 2>err.txt; then
    fatal "status in container"
fi
if ! grep -qe "error.*This system was not booted via libostree" err.txt; then
    cat err.txt
    fatal "did not find expected error"
fi

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

if ! test -x /usr/bin/yum; then
    rpm-ostree cliwrap install-to-root /
fi

# Test a critical path package
yum install cowsay && yum clean all
cowsay "It worked"
test '!' -d /var/cache/rpm-ostree

echo ok
