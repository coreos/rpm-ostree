#!/bin/bash
# Verify container build flows
set -euo pipefail

examples=(tailscale replace-kernel)
set -x

workdir=${PWD}
for example in "${examples[@]}"; do
    cd coreos-layering-examples/${example}
    # Inject our code
    tar xvf ${workdir}/install.tar
    sed -ie 's,^\(FROM .*\),\1\nADD usr/ /usr/,' Containerfile
    git diff

    # Our built binaries are using testing-devel, which might be a different
    # Fedora major version for example
    podman build --from quay.io/fedora/fedora-coreos:testing-devel -t localhost/fcos-$example .
    cd ${workdir}
done

echo ok container image integration
