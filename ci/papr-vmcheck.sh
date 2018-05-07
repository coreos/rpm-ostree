#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
OS_ID=$(. /etc/os-release && echo "${ID}")

case ${OS_ID} in
    centos)
        cat > /etc/yum.repos.d/cahc.repo <<EOF
[atomic-centos-continuous]
baseurl=https://ci.centos.org/artifacts/sig-atomic/rdgo/centos-continuous/build
gpgcheck=0
EOF
        yum -y install epel-release
        yum -y install python36;;
esac

pkg_upgrade
${dn}/build-check.sh
pkg_install git rsync openssh-clients ansible qemu-kvm standard-test-roles parallel

# "Hot patch" this to pick up https://pagure.io/standard-test-roles/pull-request/223
(cd /usr/share/ansible/inventory && curl -L -O https://pagure.io/fork/walters/standard-test-roles/raw/cpu-cores/f/inventory/standard-inventory-qcow2)

# Ensure we really have KVM loaded
if ! ls -al /dev/kvm; then
    dmesg |grep -i kvm | sed -e 's,^,dmesg: ,'
    fatal "Failed to find /dev/kvm"
fi

case ${OS_ID} in
    fedora)
        curl -Lo fedora-atomic-host.qcow2 https://getfedora.org/atomic_qcow2_latest
        export TEST_SUBJECTS="$(pwd)/fedora-atomic-host.qcow2";;
    centos)
        curl -L https://ci.centos.org/artifacts/sig-atomic/centos-continuous/images-smoketested/cloud/latest/images/centos-atomic-host-7.qcow2.gz | gunzip > centos-atomic-host-7.qcow2
        export TEST_SUBJECTS="$(pwd)/centos-atomic-host-7.qcow2";;
    *) fatal "unknown OS_ID=${OS_ID}";;
esac

make vmcheck
