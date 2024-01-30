#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
# And suppress depcheck since we didn't install via RPM
export COSA_SUPPRESS_DEPCHECK=1
ls -al /usr/bin/rpm-ostree
rpm-ostree --version
cd $(mktemp -d)
cosa init https://github.com/coreos/fedora-coreos-config/
# let's turn on opt-usrlocal-overlays in this test since CoreOS CI already
# covers the off path
echo -e '\nopt-usrlocal-overlays: true\n' >> src/config/manifest.yaml
cp /cosa/component-rpms/*.rpm overrides/rpm
# XXX: temporarily import new ostree until it makes it into FCOS
(cd overrides/rpm && curl -L --remote-name-all https://kojipkgs.fedoraproject.org//packages/ostree/2024.2/1.fc39/x86_64/ostree-{,libs-}2024.2-1.fc39.x86_64.rpm)
cosa fetch
cosa build
cosa kola run 'ext.rpm-ostree.*'
