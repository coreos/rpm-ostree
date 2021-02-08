#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

# This script is what Prow runs.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
export CC=clang CXX=clang++
${dn}/build-check.sh
