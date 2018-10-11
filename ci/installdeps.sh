#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Use the latest ostree by default
id=$(. /etc/os-release && echo $ID)
version_id=$(. /etc/os-release && echo $VERSION_ID)
if [ "$id" == fedora ] && [ "$version_id" == 28 ]; then
    echo -e '[fahc]\nmetadata_expire=1m\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/fahc/rdgo/build/\ngpgcheck=0\n' > /etc/yum.repos.d/fahc.repo
    # Until we fix https://github.com/rpm-software-management/libdnf/pull/149
    excludes='exclude=ostree ostree-libs ostree-grub2 rpm-ostree'
    for repo in /etc/yum.repos.d/fedora*.repo; do
        cat ${repo} | (while read line; do if echo "$line" | grep -qE -e '^enabled=1'; then echo "${excludes}"; fi; echo $line; done) > ${repo}.new
        mv ${repo}.new ${repo}
    done
elif [ "$id" == centos ]; then
    echo -e '[cahc]\nmetdata_expire=1m\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/rdgo/centos-continuous/build\ngpgcheck=0\n' > /etc/yum.repos.d/cahc.repo
fi

pkg_upgrade
pkg_install_builddeps rpm-ostree
# Mostly dependencies for tests
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq PyYAML \
    libubsan libasan libtsan elfutils fuse sudo python-gobject-base \
    selinux-policy-devel selinux-policy-targeted python2-createrepo_c \
    rpm-python # provided by python2-rpm on Fedora
# For ex-container tests and clang build
pkg_install_if_os fedora parallel clang

if [ -n "${CI_PKGS:-}" ]; then
  pkg_install ${CI_PKGS}
fi
