#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if [ -n "${SKIP_INSTALLDEPS:-}" ]; then
    exit 0
fi

# Add the continuous tag for latest build tools and mark as required.
version_id=$(. /etc/os-release && echo $VERSION_ID)
echo -e "[f${version_id}-coreos-continuous]\nenabled=1\nmetadata_expire=1m\nbaseurl=https://kojipkgs.fedoraproject.org/repos-dist/f${version_id}-coreos-continuous/latest/\$basearch/\ngpgcheck=0\nskip_if_unavailable=False\n" > /etc/yum.repos.d/coreos.repo

pkg_upgrade
# install base builddeps like @buildsys-build
pkg_install_builddeps
# we have the canonical spec file handy so just builddep from that
# XXX: use --allowerasing as a temporary hack to ease the migration to libmodulemd2
dnf builddep --spec -y packaging/rpm-ostree.spec.in --allowerasing
# Mostly dependencies for tests
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq python3-pyyaml \
    libubsan libasan libtsan elfutils fuse sudo python3-gobject-base \
    selinux-policy-devel selinux-policy-targeted python3-createrepo_c \
    rsync python3-rpm parallel clang rustfmt-preview
