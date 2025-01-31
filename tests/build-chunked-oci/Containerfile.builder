# Note that the GHA flow in ci.yml injects a binary from C9S.
FROM quay.io/centos-bootc/centos-bootc:stream9
RUN <<EORUN
set -xeuo pipefail
# Pull in the binary we just built; if you're doing this locally you'll want
# to e.g. run `podman build -v target/release/rpm-ostree:/ci/rpm-ostree`.
install /ci/rpm-ostree /usr/bin/
EORUN
