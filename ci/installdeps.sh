#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if [ -z "${SKIP_INSTALLDEPS:-}" ] && test $(id -u) -eq 0; then
    dnf -y install dnf-plugins-core
    # we have the canonical spec file handy so just builddep from that
    # XXX: use --allowerasing as a temporary hack to ease the migration to libmodulemd2
    time dnf builddep --spec -y packaging/rpm-ostree.spec.in --allowerasing
fi

mkdir -p target
time cargo install --root=target/cargo-vendor-filterer cargo-vendor-filterer --version ^0.5
