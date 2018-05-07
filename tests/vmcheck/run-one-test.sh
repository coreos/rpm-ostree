#!/bin/bash
# Wrap a single test-foo.sh by launching a VM,
# and grabbing the journal etc. if it fails.

set -euo pipefail

tf=$1

dn=$(cd $(dirname $0) && pwd)
. ${commondir}/libvm.sh

# Configure a subdirectory for our logs; note this also
# configures the standard-inventory-qcow2 console logs.
export TEST_ARTIFACTS=${LOGDIR}/${tf}
rm -rf ${TEST_ARTIFACTS}
mkdir -p ${TEST_ARTIFACTS}
# Redirect our stdout/stderr, since we don't want what GNU parallel does
exec 1>${TEST_ARTIFACTS}/output.txt
exec 2>&1

set -x

# Ensure we have a connection to VM, and overlay our built
# binaries.
vm_setup ${tf}
${dn}/overlay.sh

# Sanity check connection
if ! vm_ssh_wait 30; then
  echo "ERROR: A running VM is required for 'make vmcheck'."
  exit 1
fi

vm_console_log "SSH ready, preparing test: $tf"

# remember the csum we're currently on and tag it so that ostree doesn't wipe it
csum_orig=$(vm_get_booted_csum)
vm_cmd ostree rev-parse $csum_orig &> /dev/null # validate
vm_cmd ostree refs vmcheck_orig --delete
vm_cmd ostree refs $csum_orig --create vmcheck_orig

# Santiy check we're on vmcheck
origin=$(vm_get_booted_deployment_info origin)
test "$origin" = "vmcheck"

echo "Neutering /etc/yum.repos.d"
# Move the current one to .bak
vm_cmd mv /etc/yum.repos.d{,.bak}
vm_cmd mkdir /etc/yum.repos.d

if vm_cmd test -f /usr/etc/rpm-ostreed.conf; then
    vm_cmd cp -f /usr/etc/rpm-ostreed.conf /etc
    # Unless we're doing overrides
    if [[ "${VMCHECK_FLAGS:-}" =~ "stage-deployments" ]]; then
        vm_cmd 'echo "[Experimental]" >> /etc/rpm-ostreed.conf'
        vm_cmd 'echo StageDeployments=true >> /etc/rpm-ostreed.conf'
    fi
fi

origdir=$(pwd)
testdir="$(dirname $(realpath $0))"
cd $testdir

bn=$(basename ${tf})
echo "Running $bn..."
vm_cmd logger "vmcheck: running $bn..."

if ! ${dn}/${tf}; then
    echo "FAILED: ${tf}"
    echo "FAILED: ${tf}" >> ${LOGDIR}/vmcheck/failed.txt
    vm_console_log "FAILED: ${tf}" || true
    echo "Attempting to gather journal..."
    vm_cmd 'journalctl --no-pager || true' > ${TEST_ARTIFACTS}/journal.txt || true
fi
vm_console_log "SUCCESS: ${tf}"

# The standard-test-roles qemu instance will exit asynchronously, but that
# causes RAM usage spikes with parallelization in play.
vm_cmd systemd-run sh -c "'sleep 2 && systemctl halt -ff'" || true
