#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

# This script is what Prow runs.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# add cargo's directory to the PATH like we do in CoreOS CI
export PATH="$HOME/.cargo/bin:$PATH"

export CC=clang CXX=clang++
${dn}/build.sh
make check
