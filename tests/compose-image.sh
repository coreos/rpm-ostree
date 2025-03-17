#!/bin/bash
set -euo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd "$dn/.." && pwd)
commondir=$(cd "$dn/common" && pwd)
export topsrcdir commondir

# shellcheck source=common/libtest-core.sh
. "${commondir}/libtest.sh"
# Work around buggy check for overlayfs on /, but we're not writing to that
unset OSTREE_NO_XATTRS
unset OSTREE_SYSROOT_DEBUG

set -x

if test -z "${COMPOSE_KEEP_CACHE:-}"; then
    rm -rf compose-baseimage-test
    mkdir compose-baseimage-test
fi
cd compose-baseimage-test

# RELEASE is set in CI for all current Fedora releases
if [[ -z ${RELEASE+x} ]]; then
    # Set RELEASE to latest Fedora stable by default
    RELEASE=41
else
    echo "Testing using Fedora ${RELEASE}"
fi

rm -rf cache cache-container
mkdir -p cache cache-container

# A container image using stock dnf, similar to
# https://pagure.io/fedora-kickstarts/blob/main/f/fedora-container-base.ks
rm minimal-test -rf
mkdir minimal-test
cd minimal-test
dnf="dnf dnf-yum"
if [[ "${RELEASE}" -ge 41 ]]; then
    dnf="dnf5"
fi
systemd_sysusers=""
if [[ "${RELEASE}" -ge 42 ]]; then
    systemd_sysusers="  - systemd-standalone-sysusers"
fi
cat > minimal.yaml << EOF
container: true
recommends: false
releasever: ${RELEASE}
packages:
  - rootfiles
  - vim-minimal
  - coreutils
  - ${dnf}
  - glibc glibc.i686
  - sudo
${systemd_sysusers}
repos:
  - fedora  # Intentially using frozen GA repo
EOF
cp /etc/yum.repos.d/*.repo .
if rpm-ostree compose image --cachedir=../cache-container --label=foo=bar --label=baz=blah --initialize-mode=never minimal.yaml minimal.ociarchive 2>/dev/null; then
  fatal "built an image in --initialize-mode=never"
fi
rpm-ostree compose image --cachedir=../cache-container --label=foo=bar --label=baz=blah --initialize minimal.yaml minimal.ociarchive
skopeo inspect oci-archive:minimal.ociarchive > inspect.json
test $(jq -r '.Labels["foo"]' < inspect.json) = bar
test $(jq -r '.Labels["baz"]' < inspect.json) = blah
# Also verify change detection
rpm-ostree compose image --cachedir=../cache-container --touch-if-changed changed.stamp minimal.yaml minimal.ociarchive
test '!' -f changed.stamp
cd ..
echo "ok minimal ${RELEASE}"

# A minimal bootable manifest, using repos from the host
rm minimal-test -rf
mkdir minimal-test
cd minimal-test
cat > minimal.yaml << EOF
boot-location: modules
releasever: ${RELEASE}
packages:
  - bash
  - rpm
  - coreutils
  - selinux-policy-targeted
  - kernel
  - ostree
repos:
  - fedora  # Intentially using frozen GA repo
EOF
cp /etc/yum.repos.d/*.repo .
# Unfortunately, --initialize-mode=if-not-exists is broken with .ociarchive...
rpm-ostree compose image --cachedir=../cache --touch-if-changed=changed.stamp --initialize-mode=always minimal.yaml minimal.ociarchive
# TODO actually test this container image
cd ..
echo "ok minimal ${RELEASE}"

# Next, test the full Fedora Silverblue config, and also using an OCI directory
test -d workstation-ostree-config.${RELEASE} || git clone --depth=1 https://pagure.io/workstation-ostree-config --branch "f${RELEASE}" workstation-ostree-config.${RELEASE}
mkdir_oci() {
  local d
  d=$1
  shift
  mkdir $d
  echo '{ "imageLayoutVersion": "1.0.0" }' > $d/oci-layout
  echo '{ "schemaVersion": 2, "mediaType": "application/vnd.oci.image.index.v1+json", "manifests": []}' > $d/index.json
  mkdir -p $d/blobs/sha256
}
destocidir=fedora-silverblue.oci
rm "${destocidir}" -rf
mkdir_oci "${destocidir}"
destimg="${destocidir}:silverblue"
manifest="workstation-ostree-config.${RELEASE}/silverblue.yaml"
if [[ "${RELEASE}" -lt 41 ]]; then
    manifest="workstation-ostree-config.${RELEASE}/fedora-silverblue.yaml"
fi
# Sadly --if-not-exists is broken for oci: too
rpm-ostree compose image --cachedir=cache --touch-if-changed=changed.stamp --initialize-mode=always --format=oci "${manifest}" "${destimg}"
skopeo inspect "oci:${destimg}"
test -f changed.stamp
rm -f changed.stamp
rpm-ostree compose image --cachedir=cache --offline --touch-if-changed=changed.stamp --initialize-mode=if-not-exists --format=oci "${manifest}" "${destimg}"| tee out.txt
test '!' -f changed.stamp
assert_file_has_content_literal out.txt 'No apparent changes since previous commit'
echo "ok compose Silverblue ${RELEASE}"
