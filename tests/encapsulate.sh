#!/bin/bash
set -xeuo pipefail
# Pull the latest FCOS build, unpack its container image, and verify
# that we can re-encapsulate it as chunked.

# TODO: Switch to using a fixture
container=quay.io/fedora/fedora-coreos:stable

# First, verify the legacy entrypoint still works for now
rpm-ostree container-encapsulate --help >/dev/null

tmpdir=$(mktemp -d)
cd ${tmpdir}
ostree --repo=repo init --mode=bare-user
cat /etc/ostree/remotes.d/fedora.conf >> repo/config
# Pull and unpack the ostree content, discarding the container wrapping
ostree container unencapsulate --write-ref=testref --repo=repo ostree-remote-registry:fedora:$container
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
    testref oci:test.oci
skopeo inspect oci:test.oci | jq -r .Labels > labels.json
for label in foo baz 'fedora-coreos.stream' usage; do 
    jq -re ".\"${label}\"" < labels.json
done
echo ok
