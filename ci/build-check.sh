#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
# NB: avoid make function because our RPM building doesn't
# support parallel runs right now
/usr/bin/make check
make install
