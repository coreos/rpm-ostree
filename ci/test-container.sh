#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

versionid=$(. /usr/lib/os-release && echo $VERSION_ID)

# This allows running this test in a podman container locally by running
# `SELF_BOOTSTRAP=1 ci/test-container.sh`.
if [ -n "${SELF_BOOTSTRAP:-}" ]; then
  rm -rf "$PWD/installtree"
  make install DESTDIR="$PWD/installtree"
  make -C tests/kolainst install DESTDIR="$PWD/installtree"
  exec podman run -ti --rm --security-opt=label=disable -v "$PWD":/var/srv -w /var/srv \
    quay.io/fedora/fedora-coreos:testing-devel sh -c \
      'rsync -rlv installtree/ / && /var/srv/ci/test-container.sh'
fi

# Test overrides
# These hardcoded versions can be kept until Fedora GC's them
ignition_url_suffix=2.17.0/4.fc40/x86_64/ignition-2.17.0-4.fc40."$(arch)".rpm

# Detect if we're on CentOS Stream
osid=$(. /usr/lib/os-release && echo $ID)
case $versionid in
  9)
    if [ "$osid" = "centos" ]; then
      # CentOS Stream 9
      # Use CentOS Stream kernel and skip ignition tests
      koji_ignition_url=""
      koji_kernel_url=""
      kver=""
      krev=""
      is_centos=1
    else
      fatal "Unsupported version: $versionid for $osid"
    fi
    ;;
  42)
    # 2.21.0-1 (this koji url must be different than above version, and different from
    # what's in the current image)
    koji_ignition_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2681489"
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2685011"
    kver=6.14.0
    krev=63
    ;;
  41)
    # 2.19.0-2 (this koji url must be different than above version)
    koji_ignition_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2495227"
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2571615"
    kver=6.11.4
    krev=301
    ;;
  *) fatal "Unsupported version: $versionid";;
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
if [ -z "${is_centos:-}" ]; then
  rpm-ostree override replace $koji_kernel_url
  # test that the new initramfs was generated
  test -f /usr/lib/modules/${kver}-${krev}.fc${versionid}.x86_64/initramfs.img
fi

# test treefile-apply
# do it before cliwrap because we want real dnf
# Skip version locking tests on CentOS Stream 9 (no versionlock plugin)
if [ -z "${is_centos:-}" ]; then
  if rpm -q ltrace vim-enhanced; then
    fatal "ltrace and/or vim-enhanced exist"
  fi
  vim_vr=$(rpm -q vim-minimal --qf '%{version}-%{release}')
  cat > /tmp/treefile.yaml << EOF
packages:
  - ltrace
  # a split base/layered version-locked package
  - vim-enhanced
# also test repo enablement but using variables so we don't have to rewrite a
# new treefile each time
conditional-include:
  - if: addrepos == "disable"
    include:
      repos: []
  - if: addrepos == "enable"
    include:
      repos: [fedora-cisco-openh264]
      packages: [openh264-devel]
EOF
  # setting `repos` to empty list; should fail
  if rpm-ostree experimental compose treefile-apply /tmp/treefile.yaml --var addrepos=disable; then
    fatal "installed packages without enabled repos?"
  fi
  if rpm -q ltrace; then fatal "ltrace installed"; fi
  if rpm -q vim-enhanced-"$vim_vr"; then fatal "vim-enhanced installed"; fi
  # not setting repos; default enablement
  rpm-ostree experimental compose treefile-apply /tmp/treefile.yaml --var addrepos=
  rpm -q ltrace vim-enhanced-"$vim_vr"
  # setting repos; only those repos enabled
  rpm-ostree experimental compose treefile-apply /tmp/treefile.yaml --var addrepos=enable
  rpm -q openh264-devel
fi

rpm-ostree cliwrap install-to-root /

# Test a critical path package
yum -y install cowsay && yum clean all
cowsay "It worked"
test '!' -d /var/cache/rpm-ostree

rpm -e cowsay
if rpm -q cowsay; then fatal "failed to remove cowsay"; fi

# test replacement by URL
if [ -z "${is_centos:-}" ]; then
  rpm-ostree override replace $IGNITION_URL
  rpm-ostree override remove ignition
  # test local RPM install
  curl -Lo ignition.rpm $IGNITION_URL
  rpm-ostree install ignition.rpm
  rpm -q ignition
fi

# And verify it's uninstalled
dnf -y uninstall kexec-tools kdump-utils makedumpfile
if rpm -q kexec-tools; then fatal "failed to remove kexec-tools"; fi

# test replacement by Koji URL
if [ -z "${is_centos:-}" ]; then
  rpm-ostree override replace $koji_ignition_url |& tee out.txt
  n_downloaded=$(grep Downloading out.txt | wc -l)
  if [[ $n_downloaded != 1 ]]; then
    fatal "Expected 1 'Downloading', but got $n_downloaded"
  fi

  (cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/fedora-coreos-pool.repo)
  (cd /etc/yum.repos.d/ && curl -LO https://raw.githubusercontent.com/coreos/fedora-coreos-config/testing-devel/ci/continuous/fcos-continuous.repo)

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
fi

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
