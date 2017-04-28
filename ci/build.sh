#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

install_builddeps rpm-ostree

# ⚠ Pull latest ostree for https://github.com/ostreedev/ostree/issues/758
# And we now depend on 2017.4
# Also, there's a copy of this below in the compose context
# And also in tests/vmcheck/overlay.sh
yum -y install https://kojipkgs.fedoraproject.org//packages/ostree/2017.5/2.fc25/x86_64/ostree-{,libs-,devel-,grub2-}2017.5-2.fc25.x86_64.rpm

dnf install -y createrepo_c /usr/bin/jq PyYAML clang \
    libubsan libasan libtsan elfutils fuse sudo gnome-desktop-testing

# create an unprivileged user for testing
adduser testuser

build --enable-installed-tests --enable-gtk-doc
