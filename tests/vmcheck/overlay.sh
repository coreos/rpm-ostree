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

    set -x

    cd ${topsrcdir}

    # Support local development with e.g. an ostree built from git too,
    # or libasan.
    export VMCHECK_INSTTREE=${VMCHECK_INSTTREE:-$(pwd)/insttree}

    # Use a lock in case we're called in parallel (make install might fail).
    # Plus, we can just share the same install tree, and sharing is caring!
    flock insttree.lock sh -ec \
      '[ ! -d ${VMCHECK_INSTTREE} ] || exit 0
       DESTDIR=${VMCHECK_INSTTREE}
       make install DESTDIR=${DESTDIR}
       touch ${DESTDIR}/.completed'
    [ -f ${VMCHECK_INSTTREE}/.completed ]

    vm_rsync

    $SSH "env INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/overlay.sh"
    vm_reboot
    exit 0
fi

set -x

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
# ✀✀✀ BEGIN hack for https://github.com/projectatomic/rpm-ostree/pull/693 ✀✀✀
rm -f vmcheck/usr/etc/{.pwd.lock,passwd-,group-,shadow-,gshadow-,subuid-,subgid-}
# ✀✀✀ END hack for https://github.com/projectatomic/rpm-ostree/pull/693 ✀✀✀
# ✀✀✀ BEGIN update ostree; see redhat-ci.sh ✀✀✀
for url in https://kojipkgs.fedoraproject.org//packages/ostree/2017.5/2.fc25/x86_64/ostree-{,libs-,grub2-}2017.5-2.fc25.x86_64.rpm; do
    curl -sSL -O $url
done
for x in *.rpm; do
    rpm2cpio $x | (cd vmcheck && cpio -div)
done
rm vmcheck/etc -rf
rm -f *.rpm
# ✀✀✀ END update ostree; see redhat-ci.sh ✀✀✀
# Now, overlay our built binaries
rsync -rlv /var/roothome/sync/insttree/usr/ vmcheck/usr/
# ✀✀✀ BEGIN hack for https://github.com/projectatomic/rpm-ostree/pull/642 ✀✀✀
ostree admin unlock || true
for url in https://kojipkgs.fedoraproject.org//packages/ostree/2017.2/3.fc25/x86_64/ostree-{,libs-,grub2-}2017.2-3.fc25.x86_64.rpm; do
    curl -sSL -O $url
done
rpm -Uvh --force *.rpm
for x in *.rpm; do
    rpm2cpio $x | (cd vmcheck && cpio -div)
done
rm vmcheck/etc -rf
rm -f *.rpm
# ✀✀✀ END hack for https://github.com/projectatomic/rpm-ostree/pull/642 ✀✀✀
ostree refs --delete vmcheck || true
ostree commit -b vmcheck -s '' --tree=dir=vmcheck --link-checkout-speedup
ostree admin deploy vmcheck
