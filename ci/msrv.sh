#!/bin/bash
set -euo pipefail

# this corresponds to the latest Rust module available in el8
MINIMUM_SUPPORTED_RUST_VERSION=1.37.0

ci/installdeps.sh
dnf remove -y cargo
curl https://sh.rustup.rs -sSf | sh -s -- --default-toolchain ${MINIMUM_SUPPORTED_RUST_VERSION} -y
PATH="$HOME/.cargo/bin:$PATH" ci/build.sh |& tee out.txt
grep ${MINIMUM_SUPPORTED_RUST_VERSION} out.txt
grep "checking for cargo... $HOME/.cargo/bin/cargo" out.txt
grep "checking for rustc... $HOME/.cargo/bin/rustc" out.txt
