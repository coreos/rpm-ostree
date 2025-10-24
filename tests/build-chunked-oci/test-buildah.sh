#!/bin/bash
set -euxo pipefail

cat > Containerfile <<EOF
FROM quay.io/centos-bootc/centos-bootc:stream10
RUN dnf install -y vim-enhanced
EOF

buildah pull quay.io/centos-bootc/centos-bootc:stream10
buildah build -t localhost/my-image .
/usr/bin/rpm-ostree experimental compose build-chunked-oci --bootc --from=localhost/my-image --output=containers-storage:localhost/my-image-chunked --format-version=2

# Sanity check: verify ostree labels are present
echo "Checking for ostree labels..."
if ! skopeo inspect containers-storage:localhost/my-image-chunked | grep -q '"ostree\.'; then
    echo "ERROR: No ostree labels found in output image"
    exit 1
fi

echo "SUCCESS: Output image exists and contains ostree labels"
