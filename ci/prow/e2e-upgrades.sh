#!/bin/bash
set -xeuo pipefail

# Attempt to keep this script in sync with https://github.com/containers/bootc/blob/main/ci/run-kola.sh

# We require the an image containing our binaries-under-test to have been injected
# by an external system, e.g. Prow 
# https://docs.ci.openshift.org/docs/architecture/ci-operator/#referring-to-images-in-tests

tmpdir="$(mktemp -d -p /var/tmp)"
cd "${tmpdir}"

echo "ostree-unverified-registry:$TARGET_IMAGE" > target-image
echo "ostree-unverified-registry:$UPGRADE_IMAGE" > upgrade-image
# Detect Prow; if we find it, assume the image requires a pull secret
if test -n "${JOB_NAME_HASH:-}"; then
    oc registry login --to auth.json
else
    # Default to an empty secret to exercise that path
    echo '{}' > auth.json
fi
cat > config.bu << 'EOF'
variant: fcos
version: 1.1.0
storage:
  files:
    - path: /etc/target-image
      contents:
        local: target-image
    - path: /etc/upgrade-image
      contents:
        local: upgrade-image
    - path: /etc/ostree/auth.json
      contents:
        local: auth.json
EOF
butane -d . < config.bu > config.ign

if test -z "${BASE_QEMU_IMAGE:-}"; then
    coreos-installer download -p qemu -f qcow2.xz --decompress
    BASE_QEMU_IMAGE=./"$(echo *.qcow2)"
fi
cosa kola run --append-ignition config.ign --oscontainer ostree-unverified-registry:${TARGET_IMAGE} --qemu-image "${BASE_QEMU_IMAGE}" ext.rpm-ostree.upgrades

echo "ok kola upgrades"
