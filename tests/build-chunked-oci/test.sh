#!/bin/bash
set -xeuo pipefail

# First: a cross-arch rechunking
testimg_base=quay.io/centos-bootc/centos-bootc:stream9
chunked_output=localhost/chunked-ppc64le
podman pull --arch=ppc64le ${testimg_base}
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from ${testimg_base} --output containers-storage:${chunked_output} --label foo=bar
podman rmi ${testimg_base}
podman inspect containers-storage:${chunked_output} | jq '.[0]' > config.json
podman rmi ${chunked_output}
test $(jq -r '.Architecture' < config.json) = "ppc64le"
test $(jq -r '.Labels.foo' < config.json) = "bar"
echo "ok cross arch rechunking"

# Build a custom image, then rechunk it
podman build -t localhost/base -f Containerfile.test
orig_created=$(podman inspect containers-storage:localhost/base | jq -r '.[0].Created')
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --format-version=1 --max-layers 99 --from localhost/base --output containers-storage:localhost/chunked
podman inspect containers-storage:localhost/chunked | jq '.[0]' > new-config.json
# Verify we propagated the creation date
new_created=$(jq -r .Created < new-config.json)
# ostree only stores seconds, so canonialize the rfc3339 data to seconds
test "$(date --date="${orig_created}" --rfc-3339=seconds)" = "$(date --date="${new_created}" --rfc-3339=seconds)"
# Verify we propagated labels
test $(jq -r .Labels.testlabel < new-config.json) = "1"
echo "ok rechunking with labels"

# Verify directory metadata for --format-version=1 image
# This will have nondeterministic mtimes creep in
test "$(podman run --rm containers-storage:localhost/chunked find /usr -newermt @0 | wc -l)" -gt 0

# Build a rechunked image with --format-version=2
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --format-version=2 --max-layers 99 --from localhost/base --output containers-storage:localhost/chunked

# Verify directory metadata for --format-version=2 image
# This will have deterministic mtimes
test "$(podman run --rm containers-storage:localhost/chunked find /usr -newermt @0 | wc -l)" -eq 0

# Test chunked image base detection and reuse
# First create a chunked image from base
echo "Testing rechunking existing chunked image"
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from localhost/base --output containers-storage:localhost/chunked

# Get the layer digests from the original chunked image
original_layers_file=$(mktemp)
podman inspect containers-storage:localhost/chunked | jq -r '.[0].RootFS.Layers[]' | sort > "$original_layers_file"

# Build a modified image from the chunked base that adds new packages
cat > Containerfile.modified <<EOF
FROM localhost/chunked
RUN dnf install -y vim-enhanced git-core postgresql
EOF

podman build -t localhost/modified -f Containerfile.modified

# Re-chunk the modified image - this should detect and reuse the chunked base
echo "Re-chunking using existing chunked image"
rechunk_output=$(mktemp)
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from localhost/modified --output containers-storage:localhost/chunked 2>&1 | tee "$rechunk_output"

# Check that the expected output string is present
if ! grep -q "Found existing chunked image at target, will use as baseline" "$rechunk_output"; then
    echo "ERROR: Expected output 'Found existing chunked image at target, will use as baseline' not found"
    exit 1
fi

# Get the layer digests from the rechunked image
rechunked_layers_file=$(mktemp)
podman inspect containers-storage:localhost/chunked | jq -r '.[0].RootFS.Layers[]' | sort > "$rechunked_layers_file"

# Verify that some layers from the original chunked image are reused
# The rechunked image should contain most of the original layers plus new ones
common_layers=$(comm -12 "$original_layers_file" "$rechunked_layers_file" | wc -l)
original_layer_count=$(wc -l < "$original_layers_file")
rechunked_layer_count=$(wc -l < "$rechunked_layers_file")

echo "original_layer_count: $original_layer_count"
echo "rechunked_layer_count: $rechunked_layer_count"
echo "common_layers: $common_layers"

# At least 80% of original layers should be reused
min_reused=$((original_layer_count * 8 / 10))
test "$common_layers" -ge "$min_reused"

# The rechunked image should have more layers (due to new packages)
test "$rechunked_layer_count" -ge "$original_layer_count"

# Verify the rechunked image has the expected packages
podman run --rm containers-storage:localhost/chunked rpm -q vim-enhanced git-core postgresql

# Cleanup temporary files
rm -f "$original_layers_file" "$rechunked_layers_file" "$rechunk_output"

echo "ok chunked image base detection and reuse"

echo "Testing exclusive layers functionality"
podman build -t localhost/exclusive-test -f Containerfile.recursive

podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci \
  --bootc --format-version=2 --max-layers 99 \
  --from localhost/exclusive-test \
  --output containers-storage:localhost/exclusive-chunked

verify_layer_contents() {
    local component_name="$1"
    local expected_files="$2"
    local image_manifest="$3"

    local layer_index
    layer_index=$(jq -r --arg comp "$component_name" '.layers | to_entries[] | select(.value.annotations."ostree.components" == $comp) | .key' exclusive-manifest.json)
    echo "$component_name layer index: $layer_index"
    local layer_digest
    layer_digest=$(echo "$image_manifest" | jq -r --argjson idx "$layer_index" '.layers[$idx].digest' | cut -d: -f2)
    
    if [ -n "$layer_digest" ]; then
        echo "Checking $component_name layer: sha256:$layer_digest"
        
        # List files in the tar (excluding directories and sysroot)
        local actual_files
        actual_files=$(tar -tf "${oci_dir}/blobs/sha256/${layer_digest}" | grep -v '/$' | grep -v '^sysroot' | sort)
        
        if [ "$actual_files" = "$expected_files" ]; then
            echo "✓ $component_name layer contains only expected files"
        else
            echo "✗ $component_name layer contents mismatch"
            echo "Expected:"
            echo "$expected_files"
            echo "Actual:"
            echo "$actual_files"
            exit 1
        fi
    else
        echo "✗ $component_name layer not found"
        exit 1
    fi
}

# Verify that exclusive layers contain only the expected files
echo "Verifying exclusive layer contents..."

skopeo inspect --raw containers-storage:localhost/exclusive-chunked > exclusive-manifest.json

webapp_index=$(jq -r '.layers | to_entries[] | select(.value.annotations."ostree.components" == "webapp") | .key' exclusive-manifest.json)
database_index=$(jq -r '.layers | to_entries[] | select(.value.annotations."ostree.components" == "database") | .key' exclusive-manifest.json)

echo "webapp layer index: $webapp_index"
echo "database layer index: $database_index"

oci_dir=$(mktemp -d)
skopeo copy containers-storage:localhost/exclusive-chunked "oci:${oci_dir}" &> /dev/null
manifest=$(cat "${oci_dir}/index.json" | jq -r '.manifests[0].digest' | cut -d: -f2)
image_manifest=$(cat "${oci_dir}/blobs/sha256/${manifest}")

expected_root="usr
usr/share
usr/share/layers
usr/share/layers/broken-linkB
usr/share/layers/dir2
usr/share/layers/dir2/dirA
usr/share/layers/dir2/dirA/fileA
usr/share/layers/dir2/fileA
usr/share/layers/dir2/linkA
usr/share/layers/dir2/targetA
usr/share/layers/dir2/targetB
usr/share/layers/dir3
usr/share/layers/dir3/fileA
usr/share/layers/fileA
usr/share/layers/linkA
usr/share/layers/linkB
usr/share/layers/targetA"

expected_dir1="usr
usr/share
usr/share/layers
usr/share/layers/dir1
usr/share/layers/dir1/dirA
usr/share/layers/dir1/dirA/fileA
usr/share/layers/dir1/linkA
usr/share/layers/dir1/linkB
usr/share/layers/dir1/targetA
usr/share/layers/dir1/targetB"

expected_dir3fileB="usr
usr/share
usr/share/layers
usr/share/layers/dir3
usr/share/layers/dir3/fileB"

expected_dir4="usr
usr/share
usr/share/layers
usr/share/layers/dir4
usr/share/layers/dir4/fileA"

expected_dir4fileB="usr
usr/share
usr/share/layers
usr/share/layers/dir4
usr/share/layers/dir4/fileB"

# Verify each layer using the shared function
verify_layer_contents "root" "$expected_root" "$image_manifest"
verify_layer_contents "dir1" "$expected_dir1" "$image_manifest"
verify_layer_contents "dir3fileB" "$expected_dir3fileB" "$image_manifest"
verify_layer_contents "dir4" "$expected_dir4" "$image_manifest"
verify_layer_contents "dir4fileB" "$expected_dir4fileB" "$image_manifest"

echo "ok exclusive layers functionality"

echo "Testing oci-archive output"
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from localhost/base --output=oci-archive:/output/test-archive

test -f test-archive
echo "ok oci-archive output"
