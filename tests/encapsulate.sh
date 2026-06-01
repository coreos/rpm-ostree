#!/bin/bash
set -xeuo pipefail
# Pull an ostree commit and verify that we can encapsulate it
# as a chunked container image.

# First, verify the legacy entrypoint still works for now
rpm-ostree container-encapsulate --help >/dev/null

tmpdir=$(mktemp -d)
cd ${tmpdir}
ostree --repo=repo init --mode=bare-user
cat /etc/ostree/remotes.d/fedora.conf >> repo/config
# Pull the ostree commit directly from the remote. We use ostree pull
# rather than container unencapsulate because FCOS stable now ships
# non-ostree layers for bootc compatibility.
testref=fedora:fedora/x86_64/coreos/stable
ostree --repo=repo pull "${testref}"
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
