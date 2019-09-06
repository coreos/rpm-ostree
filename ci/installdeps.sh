#!/usr/bin/bash
# Install build dependencies

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Use the latest ostree by default (XXX: currently pulling in f29 ostree, need
# to bump rdgo to f30 or wait for packit)
id=$(. /etc/os-release && echo $ID)
version_id=$(. /etc/os-release && echo $VERSION_ID)
if [ "$id" == fedora ] && [ "$version_id" -ge 29 ]; then
    echo -e '[fahc]\nmetadata_expire=1m\nbaseurl=https://ci.centos.org/artifacts/sig-atomic/fahc/rdgo/build/\ngpgcheck=0\n' > /etc/yum.repos.d/fahc.repo
    # Until we fix https://github.com/rpm-software-management/libdnf/pull/149
    excludes='exclude=ostree ostree-libs ostree-grub2 rpm-ostree'
    for repo in /etc/yum.repos.d/fedora*.repo; do
        cat ${repo} | (while IFS= read -r line; do if echo "$line" | grep -qE -e '^enabled=1'; then echo "${excludes}"; fi; echo "$line"; done) > ${repo}.new
        mv ${repo}.new ${repo}
    done
fi

pkg_upgrade
pkg_install_builddeps rpm-ostree
# and we have the canonical spec file handy so just builddep from that too
dnf builddep --spec -y packaging/rpm-ostree.spec.in
# Mostly dependencies for tests
pkg_install ostree{,-devel,-grub2} createrepo_c /usr/bin/jq python3-pyyaml \
    libubsan libasan libtsan elfutils fuse sudo python3-gobject-base \
    selinux-policy-devel selinux-policy-targeted python3-createrepo_c \
    rsync python3-rpm parallel clang rustfmt-preview
