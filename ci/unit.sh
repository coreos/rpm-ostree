#!/bin/bash
set -euo pipefail

# Usually, we try to keep this to no newer than current RHEL8 rust-toolset version.
# You can find the current versions from here:
# https://access.redhat.com/documentation/en-us/red_hat_developer_tools/1/
# However, right now we are bumping to 1.48 so we can use https://cxx.rs
MINIMUM_SUPPORTED_RUST_VERSION=1.48

ci/installdeps.sh
dnf remove -y cargo rust
curl https://sh.rustup.rs -sSf | sh -s -- --default-toolchain ${MINIMUM_SUPPORTED_RUST_VERSION} -y
export PATH="$HOME/.cargo/bin:$PATH"
ci/build.sh
cargo +${MINIMUM_SUPPORTED_RUST_VERSION} test
