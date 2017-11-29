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

# get csum and origin of current default deployment
commit=$(rpm-ostree status --json | \
  python -c '
import sys, json;
deployment = json.load(sys.stdin)["deployments"][0]
print deployment["checksum"]
exit()')
origin=$(rpm-ostree status --json | \
  python -c '
import sys, json;
deployment = json.load(sys.stdin)["deployments"][0]
print deployment["origin"]
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

# ✀✀✀ BEGIN hack to get --selinux-policy (https://github.com/ostreedev/ostree/pull/1114) ✀✀✀
if ! ostree commit --help | grep -q -e --selinux-policy; then
  # this is fine, rsync doesn't modify in place
  mount -o rw,remount /usr
  # don't overwrite /etc/ to not mess up 3-way merge
  rsync -rlv --exclude '/etc/' vmcheck/usr/ /usr/
fi
# ✀✀✀ END hack to get --selinux-policy ✀✀✀

# ✀✀✀ BEGIN tmp hack for https://github.com/projectatomic/rpm-ostree/pull/999
rm -vrf vmcheck/usr/etc/selinux/targeted/semanage.*.LOCK
# ✀✀✀ END tmp hack

ostree commit --parent=none -b vmcheck \
       --add-metadata-string=ostree.source-title="Dev overlay on ${origin}" \
       --add-metadata-string=rpmostree.original-origin=${origin} \
       --link-checkout-speedup --consume \
       --selinux-policy=vmcheck --tree=dir=vmcheck
ostree admin deploy vmcheck
