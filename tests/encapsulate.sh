#!/bin/bash
set -xeuo pipefail
# Pull the latest FCOS build, unpack its container image, and verify
# that we can re-encapsulate it as chunked.

container=quay.io/coreos-assembler/fcos:testing-devel

tmpdir=$(mktemp -d)
cd ${tmpdir}
ostree --repo=repo init --mode=bare-user
cat /etc/ostree/remotes.d/fedora.conf >> repo/config
# Pull and unpack the ostree content, discarding the container wrapping
ostree container unencapsulate --write-ref=testref --repo=repo ostree-remote-registry:fedora:$container
# Re-pack it as a (chunked) container
rpm-ostree container-encapsulate --repo=repo \
    --label=foo=bar --label baz=blah \
    testref oci:test.oci
skopeo inspect oci:test.oci | jq -r .Labels.foo > labels.txt
grep -qFe bar labels.txt
echo ok
