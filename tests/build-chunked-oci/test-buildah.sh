#!/bin/bash
set -euxo pipefail

buildah build -t localhost/my-image .
rpm-ostree experimental compose build-chunked-oci --bootc --from=localhost/my-image --output=containers-storage:localhost/my-image-chunked --format-version=2
