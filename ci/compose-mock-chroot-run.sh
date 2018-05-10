#!/usr/bin/bash
#
# Host-side of compose-mock-chroot.sh

set -xeuo pipefail

rpm-ostree install mock rsync
rpm-ostree ex livefs

mockopts="--old-chroot -r ${MOCKROOT}"
mock ${mockopts} --init --install ostree rpm-ostree
rsync -rlv ~/insttree/ /var/lib/mock/${MOCKROOT}/root/
mock ${mockopts} --chroot 'rpm-ostree compose tree --test-compose-capability'
echo "OK"
