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
cat > Containerfile.exclusive << 'EOF'
FROM localhost/base

# Create directories for component files
RUN mkdir -p /usr/share/webapp /usr/share/database /usr/share/regular

# Create component files
RUN echo "Web application data" > /usr/share/webapp/app.js && \
    echo "Web configuration" > /usr/share/webapp/config.json && \
    echo "Database schema" > /usr/share/database/schema.sql && \
    echo "Database library" > /usr/share/database/libdb.so && \
    echo "Regular system file" > /usr/share/regular/system.conf

# Set component xattrs to identify exclusive layers
RUN setfattr -n user.component -v "webapp" /usr/share/webapp/app.js && \
    setfattr -n user.component -v "webapp" /usr/share/webapp/config.json && \
    setfattr -n user.component -v "database" /usr/share/database/schema.sql && \
    setfattr -n user.component -v "database" /usr/share/database/libdb.so

LABEL exclusive-test=1
EOF

podman build -t localhost/exclusive-test -f Containerfile.exclusive

podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v "$(pwd)":/output \
  localhost/builder rpm-ostree compose build-chunked-oci \
  --bootc --format-version=2 --max-layers 99 \
  --from localhost/exclusive-test \
  --output containers-storage:localhost/exclusive-chunked

# Verify that exclusive layers contain only the expected files
echo "Verifying exclusive layer contents..."

skopeo inspect --raw containers-storage:localhost/exclusive-chunked > exclusive-manifest.json

webapp_index=$(jq -r '.layers | to_entries[] | select(.value.annotations."ostree.components" == "webapp") | .key' exclusive-manifest.json)
database_index=$(jq -r '.layers | to_entries[] | select(.value.annotations."ostree.components" == "database") | .key' exclusive-manifest.json)

echo "webapp layer index: $webapp_index"
echo "database layer index: $database_index"

oci_dir=$(mktemp -d)
skopeo copy containers-storage:localhost/exclusive-chunked oci:${oci_dir} &> /dev/null
manifest=$(cat "${oci_dir}/index.json" | jq -r '.manifests[0].digest' | cut -d: -f2)
image_manifest=$(cat "${oci_dir}/blobs/sha256/${manifest}")
webapp_layer=$(echo "$image_manifest" | jq -r --argjson idx "$webapp_index" '.layers[$idx].digest' | cut -d: -f2)

if [ -n "$webapp_layer" ]; then
    echo "Checking webapp layer: sha256:$webapp_layer"
    
    # List files in the tar (excluding directories and sysroot)
    webapp_files=$(tar -tf "${oci_dir}/blobs/sha256/${webapp_layer}" | grep -v '^sysroot' | sort)
    expected_webapp="usr
usr/share
usr/share/webapp
usr/share/webapp/app.js
usr/share/webapp/config.json"
    
    if [ "$webapp_files" = "$expected_webapp" ]; then
        echo "✓ Webapp layer contains only expected files"
    else
        echo "✗ Webapp layer contents mismatch"
        echo "Expected:"
        echo "$expected_webapp"
        echo "Actual:"
        echo "$webapp_files"
        exit 1
    fi
else 
    echo "✗ webapp layer not found"
    exit 1
fi

database_layer=$(echo "$image_manifest" | jq -r --argjson idx "$database_index" '.layers[$idx].digest' | cut -d: -f2)
echo "database_layer: $database_layer"
if [ -n "$database_layer" ]; then
    echo "Checking database layer: sha256:$database_layer"
    
    # List files in the tar (excluding directories and sysroot)
    database_files=$(tar -tf "${oci_dir}/blobs/sha256/${database_layer}" | grep -v '/$' | grep -v '^sysroot' | sort)
    expected_database="usr
usr/share
usr/share/database
usr/share/database/libdb.so
usr/share/database/schema.sql"
    
    if [ "$database_files" = "$expected_database" ]; then
        echo "✓ Database layer contains only expected files"
    else
        echo "✗ Database layer contents mismatch"
        echo "Expected:"
        echo "$expected_database"
        echo "Actual:"
        echo "$database_files"
        exit 1
    fi
else 
    echo "✗ database layer not found"
    exit 1
fi

echo "ok exclusive layers functionality"
