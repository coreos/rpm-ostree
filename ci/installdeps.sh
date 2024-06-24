#!/usr/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if [ -z "${SKIP_INSTALLDEPS:-}" ] && test $(id -u) -eq 0; then
    dnf -y install dnf-plugins-core
    # we have the canonical spec file handy so just builddep from that
    # XXX: use --allowerasing as a temporary hack to ease the migration to libmodulemd2
    time dnf builddep --spec -y packaging/rpm-ostree.spec --allowerasing

    osid="$(. /etc/os-release && echo $ID)"
    if [ "${osid}" == centos ]; then
        dnf -y update https://kojihub.stream.centos.org/kojifiles/packages/ostree/2023.7/2.el9/$(arch)/ostree-{,libs-,devel-}2023.7-2.el9.$(arch).rpm
    fi
fi

mkdir -p target
time cargo install --root=target/cargo-vendor-filterer cargo-vendor-filterer --version ^0.5
