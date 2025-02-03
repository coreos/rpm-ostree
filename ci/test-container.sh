#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

versionid=$(. /usr/lib/os-release && echo $VERSION_ID)

# Test overrides
case $versionid in
  41)
    ignition_url_suffix=2.17.0/4.fc40/x86_64/ignition-2.17.0-4.fc40.x86_64.rpm
    # 2.19.0-2 (this koji url must be different than above version)
    koji_ignition_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2495227"
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2571615"
    kver=6.11.4
    krev=301
    ;;
  40)
    ignition_url_suffix=2.16.2/2.fc39/x86_64/ignition-2.16.2-2.fc39.x86_64.rpm
    # 2.15.0-3
    koji_ignition_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2158585"
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2436096"
    kver=6.8.5
    krev=301
    ;;
  *) fatal "Unsupported Fedora version: $versionid";;
esac
IGNITION_URL=https://kojipkgs.fedoraproject.org//packages/ignition/$ignition_url_suffix

repodir=/usr/lib/coreos-assembler/tests/kola/rpm-ostree/destructive/data/rpm-repos/

cat >/etc/yum.repos.d/libtest.repo <<EOF
[libtest]
name=libtest repo
baseurl=file://${repodir}/0
gpgcheck=0
enabled=1
EOF

# Verify container flow
if rpm-ostree status 2>err.txt; then
    fatal "status in container"
fi
if ! grep -qe "error.*This system was not booted via libostree" err.txt; then
    cat err.txt
    fatal "did not find expected error"
fi

# This does nothing, just verifies the experimental CLI works.
rpm-ostree experimental stub >out.txt
grep 'Did nothing successfully' out.txt && rm out.txt

rpm-ostree install testdaemon
grep -qF 'u testdaemon-user' /usr/lib/sysusers.d/35-rpmostree-pkg-user-testdaemon-user.conf
grep -qF 'g testdaemon-group' /usr/lib/sysusers.d/30-rpmostree-pkg-group-testdaemon-group.conf

origindir=/etc/rpm-ostree/origin.d
mkdir -p "${origindir}"
cat > "${origindir}/clienterror.yaml" << 'EOF'
base-refspec: "foo/x86_64/bar"
EOF
if rpm-ostree ex rebuild 2>err.txt; then
   fatal "did rebuild with base-refspec"
fi

# Test the no-op rebuild path
rm "${origindir}/clienterror.yaml"
rpm-ostree ex rebuild

# test kernel installs *before* enabling cliwrap
rpm-ostree override replace $koji_kernel_url
# test that the new initramfs was generated
test -f /usr/lib/modules/${kver}-${krev}.fc${versionid}.x86_64/initramfs.img

rpm-ostree cliwrap install-to-root /

# Test a critical path package
yum -y install cowsay && yum clean all
cowsay "It worked"
test '!' -d /var/cache/rpm-ostree

rpm -e cowsay
if rpm -q cowsay; then fatal "failed to remove cowsay"; fi

# test replacement by URL
rpm-ostree override replace $IGNITION_URL
rpm-ostree override remove ignition
# test local RPM install
curl -Lo ignition.rpm $IGNITION_URL
rpm-ostree install ignition.rpm
rpm -q ignition

# And verify it's uninstalled
dnf -y uninstall kexec-tools kdump-utils makedumpfile
if rpm -q kexec-tools; then fatal "failed to remove kexec-tools"; fi

# test replacement by Koji URL
rpm-ostree override replace $koji_ignition_url |& tee out.txt
n_downloaded=$(grep Downloading out.txt | wc -l)
if [[ $n_downloaded != 1 ]]; then
  fatal "Expected 1 'Downloading', but got $n_downloaded"
fi

(cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/fedora-coreos-pool.repo)
(cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/ci/continuous/fcos-continuous.repo)

# test repo override by NEVRA
afterburn_version=5.5.1-1.fc40."$(arch)"
rpm-ostree override replace --experimental --from repo=fedora-coreos-pool \
  afterburn-{,dracut-}"${afterburn_version}"

rpm -q afterburn-{,dracut-}"${afterburn_version}"

# test repo override by pkgname, and also test --install
if rpm -q strace; then
  echo "strace should not be installed"; exit 1
fi
rpm-ostree override replace --install strace --experimental \
  --from repo=copr:copr.fedorainfracloud.org:group_CoreOS:continuous \
  afterburn \
  afterburn-dracut
rpm -q strace

# the continuous build's version has the git rev, prefixed with g
rpm -q afterburn | grep g
rpm -q afterburn-dracut | grep g

# test --enablerepo --disablerepo --releasever
rpm-ostree --releasever=40 --disablerepo="*" \
    --enablerepo=fedora install tmux
rpm -q tmux-3.4-1.fc40."$(arch)"

# test skipping cliwraps
export RPMOSTREE_CLIWRAP_SKIP=1
if yum swap foo 2>err.txt; then
    fatal "skipping cliwrap"
fi
if ! grep -qe "error: No such file or directory" err.txt; then
    cat err.txt
    fatal "did not find expected error when skipping CLI wraps."
fi

# test treefile-apply
if rpm -q ltrace vim-enhanced; then
  fatal "ltrace and/or vim-enhanced exist"
fi
vim_vr=$(rpm -q vim-minimal --qf '%{version}-%{release}')
cat > /tmp/treefile.yaml << EOF
packages:
  - ltrace
  # a split base/layered version-locked package
  - vim-enhanced
EOF
rpm-ostree experimental compose treefile-apply /tmp/treefile.yaml
rpm -q ltrace vim-enhanced-"$vim_vr"

echo ok
