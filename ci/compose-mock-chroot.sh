#!/usr/bin/bash
# Testing that a "compose tree" works inside mock --old-chroot and
# mock --new-chroot on CentOS 7

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh

pkg_install rsync
env topsrcdir=$(pwd) ./tests/vmcheck/install.sh

scp ci/compose-mock-chroot-run.sh ${TARGET_HOST}:
rsync -rlv $(pwd)/insttree ${TARGET_HOST}:
ssh ${TARGET_HOST} env MOCKROOT=${MOCKROOT} ./compose-mock-chroot-run.sh
