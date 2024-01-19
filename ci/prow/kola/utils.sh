#!/bin/bash

. /etc/os-release
case $VERSION_ID in
  39) kernel_release=6.5.6-300.fc39.x86_64
    koji_kernel_url="https://koji.fedoraproject.org/koji/buildinfo?buildID=2302642"
  ;;
  *) echo "Unsupported Fedora version: $VERSION_ID"
    exit 1
  ;;
esac

get_deployment_root() {
  local csum
  local serial
  local osname
  csum=$(rpm-ostree status --json | jq -r '.deployments[0].checksum')
  serial="$(rpm-ostree status --json | jq -r '.deployments[0].serial')"
  osname="$(rpm-ostree status --json | jq -r '.deployments[0].osname')"
  echo /ostree/deploy/$osname/deploy/$csum.$serial
}

do_testing() {
  # override kernel
  # copy test code from test-override-kernel.sh
  current=$(rpm-ostree status --json | jq -r '.deployments[0].checksum')
  rpm-ostree db list "${current}" > current-dblist.txt
  if grep -qF $kernel_release current-dblist.txt; then
    echo "Should not find $kernel_release in current deployment"
    exit 1
  fi

  grep -E '^ kernel-[0-9]' current-dblist.txt  | sed -e 's,^ *,,' > orig-kernel.txt
  test "$(wc -l < orig-kernel.txt)" == "1"
  orig_kernel=$(cat orig-kernel.txt)

  rpm-ostree override replace $koji_kernel_url
  new=$(rpm-ostree status --json | jq -r '.deployments[0].checksum')
  rpm-ostree db list "${new}" > new-dblist.txt
  if ! grep -qF $kernel_release new-dblist.txt; then
    echo "Should find $kernel_release in the new deployment"
    exit 1
  fi

  if grep -q -F -e "${orig_kernel}" new-dblist.txt; then
    echo "Should not find ${orig_kernel} in the new deployment"
    exit 1
  fi
  newroot=$(get_deployment_root)
  find ${newroot}/usr/lib/modules -maxdepth 1 -type d > modules-dirs.txt
  test "$(wc -l < modules-dirs.txt)" == "2"
  if ! grep -qF $kernel_release modules-dirs.txt; then
    echo "Should find $kernel_release in ${newroot}/usr/lib/modules"
    exit 1
  fi

  rpm-ostree kargs --append foo=bar

  touch /etc/foobar.conf
  rpm-ostree initramfs --enable --arg=-I --arg=/etc/foobar.conf

  rpm-ostree override remove vim-minimal
  rpm-ostree install vim-filesystem
}

do_checking() {
  test "$(uname -r)" == $kernel_release

  cat /proc/cmdline > cmdlinekargs.txt
  if ! grep foo=bar cmdlinekargs.txt; then
    echo "can not find kernel parameter foo=bar"
    exit 1
  fi

  lsinitrd "/usr/lib/modules/$(uname -r)/initramfs.img" > lsinitrd.txt
  if ! grep etc/foobar.conf lsinitrd.txt; then
    echo "can not find file expected to be included in initramfs.img"
    exit 1
  fi

  if rpm -q vim-minimal 2>/dev/null; then
    echo "found package expected to be removed"
    exit 1
  fi

  if ! rpm -q vim-filesystem; then
    echo "can not find package expected to be installed"
    exit 1
  fi
}
