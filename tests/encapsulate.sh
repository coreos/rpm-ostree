#!/bin/bash
set -xeuo pipefail
# Pull the latest FCOS build, unpack its container image, and verify
# that we can re-encapsulate it as chunked.

container=quay.io/fedora/fedora-coreos:testing-devel

# First, verify the legacy entrypoint still works for now
rpm-ostree container-encapsulate --help >/dev/null

tmpdir=$(mktemp -d)
cd ${tmpdir}
ostree --repo=repo init --mode=bare-user
cat /etc/ostree/remotes.d/fedora.conf >> repo/config
# Pull and unpack the ostree content, discarding the container wrapping
ostree container unencapsulate --write-ref=testref --repo=repo ostree-remote-registry:fedora:$container
# Re-pack it as a (chunked) container

rpm-ostree compose container-encapsulate --repo=repo \
    --label=foo=bar --label baz=blah --copymeta-opt fedora-coreos.stream --copymeta-opt nonexistent.key \
    testref oci:test.oci
skopeo inspect oci:test.oci | jq -r .Labels > labels.json
for label in foo baz 'fedora-coreos.stream'; do 
    jq -re ".\"${label}\"" < labels.json
done
echo ok
