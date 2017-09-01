#!/bin/bash
set -euo pipefail

# Execute this code path on the host
if test -z "${INSIDE_VM:-}"; then
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required for 'make vmcheck'."
      exit 1
    fi

    vm_rsync
    vm_cmd env INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/overlay.sh
    vm_reboot
    exit 0
fi

set -x

# And then this code path in the VM

# get csum of current default deployment
commit=$(rpm-ostree status --json | \
  python -c '
import sys, json;
deployment = json.load(sys.stdin)["deployments"][0]
print deployment["checksum"]
exit()')

if [[ -z $commit ]] || ! ostree rev-parse $commit; then
  echo "Error while determining current commit" >&2
  exit 1
fi

cd /ostree/repo/tmp
rm vmcheck -rf
ostree checkout $commit vmcheck --fsync=0
rm vmcheck/etc -rf

# Now, overlay our built binaries & config files
INSTTREE=/var/roothome/sync/insttree
rsync -rlv $INSTTREE/usr/ vmcheck/usr/
if [ -d $INSTTREE/etc ]; then # on CentOS, the dbus service file is in /usr
  rsync -rlv $INSTTREE/etc/ vmcheck/usr/etc/
fi
ostree refs --delete vmcheck || true
ostree commit -b vmcheck -s '' --tree=dir=vmcheck --link-checkout-speedup
ostree admin deploy vmcheck
