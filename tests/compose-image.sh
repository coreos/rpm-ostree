#!/bin/bash
set -euo pipefail

# Pin to branch for some reproducibility
BRANCH=f38

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
mkdir -p cache cache-container

# A container image using stock dnf, similar to
# https://pagure.io/fedora-kickstarts/blob/main/f/fedora-container-base.ks
rm minimal-test -rf
mkdir minimal-test
cd minimal-test
cat > minimal.yaml << 'EOF'
container: true
recommends: false
releasever: 38
packages:
  - rootfiles
  - fedora-repos-modular
  - vim-minimal
  - coreutils
  - dnf dnf-yum
  - glibc glibc.i686
  - sudo
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
echo "ok minimal"

# A minimal bootable manifest, using repos from the host
rm minimal-test -rf
mkdir minimal-test
cd minimal-test
cat > minimal.yaml << 'EOF'
boot-location: modules
releasever: 38
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
echo "ok minimal"

# Next, test the full Fedora Silverblue config, and also using an OCI directory
test -d workstation-ostree-config || git clone --depth=1 https://pagure.io/workstation-ostree-config --branch "${BRANCH}"
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
# Sadly --if-not-exists is broken for oci: too
rpm-ostree compose image --cachedir=cache --touch-if-changed=changed.stamp --initialize-mode=always --format=oci workstation-ostree-config/fedora-silverblue.yaml "${destimg}"
skopeo inspect "oci:${destimg}"
test -f changed.stamp
rm -f changed.stamp
rpm-ostree compose image --cachedir=cache --offline --touch-if-changed=changed.stamp --initialize-mode=if-not-exists --format=oci workstation-ostree-config/fedora-silverblue.yaml "${destimg}"| tee out.txt
test '!' -f changed.stamp
assert_file_has_content_literal out.txt 'No apparent changes since previous commit'

echo "ok compose baseimage"
