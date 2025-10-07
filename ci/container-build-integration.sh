#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
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

    # Our built binaries are using c9s, which might be a different
    # version than the examples expect
    podman build --from quay.io/centos-bootc/centos-bootc:stream9 -t localhost/fcos-$example .
    cd ${workdir}
done

echo ok container image integration
