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
git clean -dfx

# And now a clang build to find unused variables, but only run on Fedora because
# the CentOS version is ancient anyway and doesn't support all the flags that
# might get passed to it.
id=$(. /etc/os-release && echo $ID)
if [ "$id" == fedora ]; then
    export CC=clang
    export CFLAGS='-Werror=unused-variable -Werror=maybe-uninitialized'
    build_default
    # don't actually run the tests, just compile them
    /usr/bin/make check TESTS=
fi
