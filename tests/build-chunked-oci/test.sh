#!/bin/bash
set -xeuo pipefail

# First: a cross-arch rechunking
testimg_base=quay.io/centos-bootc/centos-bootc:stream9
chunked_output=localhost/chunked-ppc64le
podman pull --arch=ppc64le ${testimg_base}
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v $(pwd):/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from ${testimg_base} --output containers-storage:${chunked_output}
podman rmi ${testimg_base}
podman inspect containers-storage:${chunked_output} | jq '.[0]' > config.json
podman rmi ${chunked_output}
test $(jq -r '.Architecture' < config.json) = "ppc64le"
echo "ok cross arch rechunking"

# Build a custom image, then rechunk it
podman build -t localhost/base -f Containerfile.test
orig_created=$(podman inspect containers-storage:localhost/base | jq -r '.[0].Created')
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v $(pwd):/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --format-version=1 --max-layers 99 --from localhost/base --output containers-storage:localhost/chunked
podman inspect containers-storage:localhost/chunked | jq '.[0]' > new-config.json
# Verify we propagated the creation date
new_created=$(jq -r .Created < new-config.json)
# ostree only stores seconds, so canonialize the rfc3339 data to seconds
test "$(date --date="${orig_created}" --rfc-3339=seconds)" = "$(date --date="${new_created}" --rfc-3339=seconds)"
# Verify we propagated labels
test $(jq -r .Labels.testlabel < new-config.json) = "1"
echo "ok rechunking with labels"
