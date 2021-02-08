#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

# This script is what Prow runs.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
# Hard fail on compiler warnings in CI.  We control our compiler
# version as part of the coreos-assembler buildroot and expect
# that to be clean.
CONFIGOPTS="--enable-werror" ${dn}/build.sh
make check
