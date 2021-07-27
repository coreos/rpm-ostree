#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if [ -z "${SKIP_INSTALLDEPS:-}" ] && test $(id -u) -eq 0; then
    dnf -y install jq dnf-plugins-core
    # we have the canonical spec file handy so just builddep from that
    # XXX: use --allowerasing as a temporary hack to ease the migration to libmodulemd2
    time dnf builddep --spec -y packaging/rpm-ostree.spec.in --allowerasing
fi

# cxx.rs (cxxbridge) isn't packaged in Fedora today.  It generates
# source code, which we vendor along with our dependent crates into release
# tarballs.
CXX_VER=$(cargo metadata --format-version 1 | jq -r '.packages[]|select(.name == "cxx").version')
time cargo install cxxbridge-cmd --version "${CXX_VER}"
