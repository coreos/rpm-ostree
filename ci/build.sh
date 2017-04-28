#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Use the latest ostree by default
echo -e '[fahc]\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/fahc/rdgo/build/\ngpgcheck=0\n' > /etc/yum.repos.d/fahc.repo
# See also tests/vmcheck/overlay.sh

install_builddeps rpm-ostree

dnf install -y ostree{,-devel,-grub2} createrepo_c /usr/bin/jq PyYAML clang \
    libubsan libasan libtsan elfutils fuse sudo gnome-desktop-testing

# create an unprivileged user for testing
adduser testuser

rpm -q ostree{,-devel,-grub2}
build --enable-installed-tests --enable-gtk-doc
