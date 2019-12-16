#!/bin/bash
set -euo pipefail

if test -z "${INSIDE_VM:-}"; then

    # do this in the host
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required"
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
rsync -rlv $INSTTREE/ /

restorecon -v /usr/bin/{rpm-,}ostree /usr/libexec/rpm-ostreed

overrides_dir=/etc/systemd/system/rpm-ostreed.service.d
mkdir -p $overrides_dir

# For our test suite at least, to catch things like
# https://github.com/projectatomic/rpm-ostree/issues/826
cat > $overrides_dir/fatal-warnings.conf << EOF
[Service]
Environment=G_DEBUG=fatal-warnings
EOF

# In the developer workflow, it's just not helpful to
# have the daemon auto-exit. But let's keep it as a separate
# override file to make it easy to drop if needed.
cat > $overrides_dir/no-idle-exit.conf << EOF
[Service]
Environment=RPMOSTREE_DEBUG_DISABLE_DAEMON_IDLE_EXIT=1
EOF

systemctl daemon-reload
systemctl restart rpm-ostreed
