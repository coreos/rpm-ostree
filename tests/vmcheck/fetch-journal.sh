#!/bin/bash
set -euo pipefail

. ${commondir}/libvm.sh

vm_setup

if ! vm_ssh_wait 30; then
    echo "WARNING: Failed to wait for VM to fetch journal" > ${JOURNAL_LOG}
else
    echo "Saving ${JOURNAL_LOG}"
    vm_cmd 'journalctl --no-pager || true' > ${JOURNAL_LOG}
fi
