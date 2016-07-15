#!/bin/bash
set -euo pipefail

source ${commondir}/libvm.sh

# create ssh-config if needed and export cmds
vm_setup

if [ -n "${VMCLEAN:-}" ]; then
  vm_cmd rm -rf sync
fi

vm_rsync
vm_cmd make -C sync/vagrant ofsinstall
vm_cmd systemctl daemon-reload
vm_cmd systemctl restart rpm-ostreed
