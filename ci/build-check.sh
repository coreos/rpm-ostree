#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
# NB: override parallel builds because our RPM building
# doesn't # support that right now
make check -j1
make install
gnome-desktop-testing-runner rpm-ostree
sudo --user=testuser gnome-desktop-testing-runner rpm-ostree
git clean -dfx

# And now a clang build to find unused variables
export CC=clang
export CFLAGS='-Werror=unused-variable'
build_default
