#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if [ -n "${SKIP_INSTALLDEPS:-}" ] || test $(id -u) != 0; then
    exit 0
fi

# we have the canonical spec file handy so just builddep from that
# XXX: use --allowerasing as a temporary hack to ease the migration to libmodulemd2
dnf builddep --spec -y packaging/rpm-ostree.spec.in --allowerasing
# Mostly dependencies for tests; TODO move these into the spec file
# and also put them in the cosa buildroot (or another container)
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq python3-pyyaml \
    libubsan libasan libtsan elfutils fuse sudo python3-gobject-base \
    selinux-policy-devel selinux-policy-targeted python3-createrepo_c \
    rsync python3-rpm parallel clang rustfmt-preview distribution-gpg-keys
