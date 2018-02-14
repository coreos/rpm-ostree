#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Use the latest ostree by default
id=$(. /etc/os-release && echo $ID)
version_id=$(. /etc/os-release && echo $VERSION_ID)
if [ "$id" == fedora ] && [ "$version_id" == 27 ]; then
    echo -e '[fahc]\nmetadata_expire=1m\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/fahc/rdgo/build/\ngpgcheck=0\n' > /etc/yum.repos.d/fahc.repo
    # Until we fix https://github.com/rpm-software-management/libdnf/pull/149
    sed -i -e 's,metadata_expire=6h,exclude=ostree ostree-devel ostree-libs ostree-grub2\nmetadata_expire=6h,' /etc/yum.repos.d/fedora-updates.repo
elif [ "$id" == centos ]; then
    echo -e '[cahc]\nmetdata_expire=1m\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/rdgo/centos-continuous/build\ngpgcheck=0\n' > /etc/yum.repos.d/cahc.repo
fi

pkg_upgrade
pkg_install_builddeps rpm-ostree
# Mostly dependencies for tests
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq PyYAML \
    libubsan libasan libtsan elfutils fuse sudo python-gobject-base \
    selinux-policy-devel selinux-policy-targeted python2-createrepo_c
# For ex-container tests and clang build
pkg_install_if_os fedora parallel clang

if [ -n "${CI_PKGS:-}" ]; then
  pkg_install ${CI_PKGS}
fi

# create an unprivileged user for testing
adduser testuser

export LSAN_OPTIONS=verbosity=1:log_threads=1
# And now the build
build --enable-installed-tests --enable-gtk-doc ${CONFIGOPTS:-}
