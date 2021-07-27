#!/usr/bin/bash

# OpenShift Prow jobs don't set $HOME, but we need
# one for cargo right now.
if test -z "$HOME" || test ! -w "$HOME"; then
    export HOME=$(mktemp -d -t --suffix .prowhome)
fi

pkg_upgrade() {
    echo "Running dnf -y distro-sync... $(date)"
    time dnf -y distro-sync
    echo "Done dnf -y distro-sync! $(date)"
}

make() {
    time /usr/bin/make -j ${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN)} "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    time ./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc "$@"
    time make V=1
}

pkg_install() {
    echo "Running dnf -y install... $(date)"
    time dnf -y install "$@"
    echo "Done running dnf -y install! $(date)"
}

pkg_builddep() {
    # This is sadly the only case where it's a different command
    if test -x /usr/bin/dnf; then
        time dnf builddep -y "$@"
    else
        time yum-builddep -y "$@"
    fi
}

pkg_install_builddeps() {
    pkg_install dnf-plugins-core 'dnf-command(builddep)'
    # Base buildroot (but exclude fedora-release, conflicts with -container:
    # https://bugzilla.redhat.com/show_bug.cgi?id=1649921)
    pkg_install @buildsys-build --excludepkg fedora-release
    # builddeps+runtime deps
    if [ $# -ne 0 ]; then
      pkg_builddep "$@"
      pkg_install "$@"
      time rpm -e "$@"
    fi
}
