#!/bin/bash
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

rpm -Uvh https://download.copr.fedorainfracloud.org/results/@CoreOS/continuous/fedora-38-x86_64/06105345-rpm-ostree/rpm-ostree-{libs-,}2023.5.4.g1701971d-1.fc38.x86_64.rpm

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

if ! test -x /usr/bin/yum; then
    rpm-ostree cliwrap install-to-root /
fi

# Test a critical path package
yum -y install cowsay && yum clean all
cowsay "It worked"
test '!' -d /var/cache/rpm-ostree

rpm -e cowsay
if rpm -q cowsay; then fatal "failed to remove cowsay"; fi

versionid=$(. /usr/lib/os-release && echo $VERSION_ID)
# Let's start by trying to install a bona fide module.
# NOTE: If changing this also change the layering-modules test
case $versionid in
  36) module=cri-o:1.23/default;;
  37) module=cri-o:1.24/default;;
  38) module=cri-o:1.25/default;;
  *) assert_not_reached "Unsupported Fedora version: $versionid";;
esac
rpm-ostree ex module install "${module}"

if rpm-ostree ex module uninstall "${module}" 2>err.txt; then
  assert_not_reached "not implemented"
fi
if ! grep -qFe "not yet implemented" err.txt; then
  cat err.txt
  assert_not_reached "unexpected error"
fi

# Test overrides
versionid=$(. /usr/lib/os-release && echo $VERSION_ID)
case $versionid in
  38)
    koji_ignition_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2158585"
    koji_strace_direct="https://kojipkgs.fedoraproject.org//packages/strace/6.2/1.fc38/$(arch)/strace-6.2-1.fc38.$(arch).rpm"
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2174317"
    kver=6.2.8
    krev=300
    ;;
  *) fatal "Unsupported Fedora version: $versionid";;
esac
# test local RPM install
curl -Lo strace.rpm "${koji_strace_direct}"
rpm-ostree install strace.rpm
rpm -q strace

# And verify it's uninstalled
dnf -y uninstall kexec-tools
if rpm -q kexec-tools; then fatal "failed to remove kexec-tools"; fi

# test replacement by Koji URL
rpm-ostree override replace "${koji_ignition_url}" |& tee out.txt
n_downloaded=$(grep Downloading out.txt | wc -l)
if [[ $n_downloaded != 1 ]]; then
  fatal "Expected 1 'Downloading', but got $n_downloaded"
fi

(cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/fedora-coreos-pool.repo)
(cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/ci/continuous/fcos-continuous.repo)

# test repo override by NEVRA
afterburn_version=5.4.0-1.fc38."$(arch)"
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

rpm-ostree override replace $koji_kernel_url
# test that the new initramfs was generated
test -f /usr/lib/modules/${kver}-${krev}.fc${versionid}.x86_64/initramfs.img

# test --enablerepo --disablerepo --releasever
rpm-ostree --releasever=38 --disablerepo="*" \
    --enablerepo=fedora install tmux
rpm -q tmux-3.3a-3.fc38."$(arch)"

# test skipping cliwraps
export RPMOSTREE_CLIWRAP_SKIP=1
if yum swap foo 2>err.txt; then
    fatal "skipping cliwrap"
fi
if ! grep -qe "error: No such file or directory" err.txt; then
    cat err.txt
    fatal "did not find expected error when skipping CLI wraps."
fi

echo ok
