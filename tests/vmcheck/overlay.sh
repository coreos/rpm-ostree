#!/bin/bash
set -xeuo pipefail

# Execute this code path on the host
if test -z "${OVERLAY_IN_VM:-}"; then
    topdir=$(git rev-parse --show-toplevel)
    cd ${topdir}
    rm insttree -rf
    make install DESTDIR=$(pwd)/insttree
    ssh -o User=root -F ssh-config vmcheck "env OVERLAY_IN_VM=1 ~vagrant/sync/tests/vmcheck/overlay.sh"
    exit 0
fi

# And then this code path in the VM

commit=$(rpm-ostree status --json | \
  python -c '
import sys, json;
deployments = json.load(sys.stdin)["deployments"]
for deployment in deployments:
  if deployment["booted"]:
    print deployment["checksum"]
    exit()')

if [[ -z $commit ]] || ! ostree rev-parse $commit; then
  echo "Error while determining current commit" >&2
  exit 1
fi

cd /ostree/repo/tmp
rm vmcheck -rf
ostree checkout $commit vmcheck --fsync=0
rsync -rv ~vagrant/sync/insttree/usr/ vmcheck/usr/
ostree refs --delete vmcheck || true
ostree commit -b vmcheck -s '' --tree=dir=vmcheck --link-checkout-speedup
ostree admin deploy vmcheck
systemctl reboot

