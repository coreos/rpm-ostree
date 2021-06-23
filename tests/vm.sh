#!/bin/bash
set -euo pipefail

main() {
    local cmd=${1:-}; shift || :
    if [ "${cmd}" == "spawn" ]; then
        spawn_vm "$@"
    elif [ "${cmd}" == "info" ]; then
        info_vm "$@"
    elif [ "${cmd}" == "ssh" ]; then
        ssh_vm "$@"
    elif [ "${cmd}" == "kill" ]; then
        kill_vm
    else
        echo "Usage: $0 spawn [IMAGE]"
        echo "       $0 info"
        echo "       $0 ssh"
        echo "       $0 kill"
    fi
}

spawn_vm() {
    if [ -f .kolavm/pid ]; then
        kill "$(cat .kolavm/pid)" || :
        rm .kolavm/pid
    fi

    local image=${1:-}
    if [ -z "${image}" ]; then
        if [ -n "${COSA_DIR:-}" ]; then
            image=$(cd ${COSA_DIR} && cosa meta --image-path qemu)
        elif [ -d /srv/fcos ]; then
            image=$(cd /srv/fcos && cosa meta --image-path qemu)
        fi
    fi

    rm -rf .kolavm && mkdir -p .kolavm/ssh

    exec 4> .kolavm/info.json

    env MANTLE_SSH_DIR="$PWD/.kolavm/ssh" \
        kola spawn -k -p qemu-unpriv \
            --qemu-image "$image" -v --idle \
            --json-info-fd 4 --output-dir "$PWD/.kolavm/output" &

    echo $! > .kolavm/pid

    # hack; need cleaner API for async kola spawn
    while [ ! -s .kolavm/info.json ]; do sleep 1; done

    local ssh_ip_port ssh_ip ssh_port
    ssh_ip_port=$(jq -r .public_ip .kolavm/info.json)
    ssh_ip=${ssh_ip_port%:*}
    ssh_port=${ssh_ip_port#*:}

    cat > ssh-config <<EOF
Host vmcheck
User root
HostName ${ssh_ip}
Port ${ssh_port}
StrictHostKeyChecking no
UserKnownHostsFile /dev/null
EOF

    SSH_AUTH_SOCK=$(ls .kolavm/ssh/agent.*)
    export SSH_AUTH_SOCK
    ssh -qo User=core -F ssh-config vmcheck 'sudo cp -RT {/home/core,/root}/.ssh'
}

ssh_vm() {
    if [ ! -f .kolavm/pid ]; then
        echo "error: no VM spawned; use '$0 spawn' first" >&2
        exit 1
    fi
    SSH_AUTH_SOCK=$(ls .kolavm/ssh/agent.*) exec ssh -qF ssh-config vmcheck "$@"
}

info_vm() {
    if [ ! -f .kolavm/pid ]; then
        echo "No VM spawned"
        exit 0
    fi
    SSH_AUTH_SOCK=$(ls .kolavm/ssh/agent.*)
    echo "             PID: $(cat .kolavm/pid)"
    echo "      SSH Socket: ${SSH_AUTH_SOCK}"
    echo "     SSH Command: SSH_AUTH_SOCK=${SSH_AUTH_SOCK} ssh -F ssh-config vmcheck"
    echo "  Console Output: $(ls .kolavm/output/*/console.txt)"
}

kill_vm() {
    if [ -f .kolavm/pid ]; then
        kill "$(cat .kolavm/pid)" || :
        rm .kolavm/pid
    fi
}

main "$@"
