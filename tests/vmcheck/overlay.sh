#!/bin/bash
set -euo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${commondir}/libvm.sh

if [ -z "${SKIP_INSTALL:-}" ] && [ -z "${SKIP_VMOVERLAY:-}" ]; then
  ${dn}/install.sh
fi

# Thin wrapper around `cosa dev-overlay`.

# First, we need to find the image to operate on.
if [ -n "${VMIMAGE:-}" ]; then
  src_img=${VMIMAGE}
else
  basearch=$(cosa basearch)
  cosa_builds=${COSA_BUILDS:-cosa-builds}
  cosa_buildid=${COSA_BUILDID:-latest}
  cosa_builddir=${cosa_builds}/${cosa_buildid}/${basearch}
  if [ ! -e "${cosa_builddir}/meta.json" ]; then
    fatal "No image provide (use VMIMAGE, or cosa-builds/ or COSA_BUILDS)"
  fi

  cosa_qemu_path=$(jq -er '.images.qemu.path' "${cosa_builddir}/meta.json")
  src_img=${cosa_builddir}/${cosa_qemu_path}
fi

if [ -z "${SKIP_VMOVERLAY:-}" ]; then
  # XXX: to develop
  cosa dev-overlay --src-image "${src_img}" --add-tree insttree/ \
    --output-dir vmoverlay/ --output-ref vmcheck
  target_img=vmoverlay/$(jq -er '.images.qemu.path' "vmoverlay/meta.json")
else
  target_img=${src_img}
fi

ln -sf "$(realpath ${target_img})" tests/vmcheck/image.qcow2
