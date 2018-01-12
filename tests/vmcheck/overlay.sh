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

# get details from the current default deployment
rpm-ostree status --json > json.txt
json_field() {
  field=$1; shift;
  python -c "
import sys, json;
deployment = json.load(open('json.txt'))['deployments'][0]
print deployment.get('$field', '')
exit()"
}
commit=$(json_field checksum)
origin=$(json_field origin)
version=$(json_field version)
timestamp=$(json_field timestamp)
[ -n "$timestamp" ]
timestamp=$(date -d "@$timestamp" "+%b %d %Y")
rm -f json.txt

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

commit_opts=
for opt in --consume --no-bindings; do
    if ostree commit --help | grep -q -e "${opt}"; then
        commit_opts="${commit_opts} ${opt}"
    fi
done

source_title="${origin}"
if [ -n "$version" ]; then
  source_title="${source_title} (${version}; $timestamp)"
else
  source_title="${source_title} ($timestamp)"
fi

ostree commit --parent=none -b vmcheck \
       --add-metadata-string=ostree.source-title="Dev overlay on ${source_title}" \
       --add-metadata-string=rpmostree.original-origin=${origin} \
       --link-checkout-speedup ${commit_opts} \
       --selinux-policy=vmcheck --tree=dir=vmcheck
ostree admin deploy vmcheck
