#!/bin/bash
set -xeuo pipefail
# Pull an ostree commit and verify that we can encapsulate it
# as a chunked container image.

# First, verify the legacy entrypoint still works for now
rpm-ostree container-encapsulate --help >/dev/null

tmpdir=$(mktemp -d)
cd ${tmpdir}
ostree --repo=repo init --mode=bare-user

# Create a minimal ostree commit to test encapsulation. The commit needs
# a populated rpmdb (container-encapsulate uses package data to generate
# chunked layers) and /usr/lib/modules.
mkdir -p rootfs/usr/bin rootfs/usr/lib/modules rootfs/usr/share/rpm
echo '#!/bin/sh' > rootfs/usr/bin/test-encapsulate
chmod +x rootfs/usr/bin/test-encapsulate
# Copy host rpmdb - location varies across systems
cp /usr/share/rpm/rpmdb.sqlite rootfs/usr/share/rpm/ 2>/dev/null || \
  cp /usr/lib/sysimage/rpm/rpmdb.sqlite rootfs/usr/share/rpm/
testref=testref
ostree --repo=repo commit -b "${testref}" --tree=dir=rootfs \
  --add-metadata-string=version=1.0 \
  --add-metadata-string=fedora-coreos.stream=stable
# Re-pack it as a (chunked) container

cat > config.json << 'EOF'
{
    "Env": [
      "container=oci"
    ],
    "Labels": {
      "usage": "Do not use directly. Use as a base image for daemons. Install chosen packages and 'systemctl enable' them."
    },
    "StopSignal": "SIGRTMIN+3"
}
EOF

rpm-ostree compose container-encapsulate --repo=repo \
    --image-config=config.json \
    --label=foo=bar --label baz=blah --copymeta-opt fedora-coreos.stream --copymeta-opt nonexistent.key \
    "${testref}" oci:test.oci
skopeo inspect oci:test.oci | jq -r .Labels > labels.json
for label in foo baz 'fedora-coreos.stream' usage; do 
    jq -re ".\"${label}\"" < labels.json
done
echo ok
