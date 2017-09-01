#!/bin/bash
set -euo pipefail

if test -z "${INSIDE_VM:-}"; then

    # do this in the host
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required for 'make vmcheck'."
      exit 1
    fi

    vm_rsync
    vm_cmd env INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/sync.sh
    exit 0
fi

set -x

# And then this code path in the VM

ostree admin unlock || :

# Now, overlay our built binaries & config files
INSTTREE=/var/roothome/sync/insttree
rsync -rlv $INSTTREE/usr/ /usr/
if [ -d $INSTTREE/etc ]; then # on CentOS, the dbus service file is in /usr
  rsync -rlv $INSTTREE/etc/ /etc/
fi

restorecon -v /usr/bin/ostree
restorecon -v /usr/bin/rpm-ostree
restorecon -v /usr/libexec/rpm-ostreed
mkdir -p /etc/systemd/system/rpm-ostreed.service.d

# For our test suite at least, to catch things like
# https://github.com/projectatomic/rpm-ostree/issues/826
cat > /etc/systemd/system/rpm-ostreed.service.d/fatal-warnings.conf << EOF
[Service]
Environment=G_DEBUG=fatal-warnings
EOF

systemctl daemon-reload
systemctl restart rpm-ostreed
