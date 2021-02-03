#!/usr/bin/bash

# OpenShift Prow jobs don't set $HOME, but we need
# one for cargo right now.
if test -z "$HOME" || test ! -w "$HOME"; then
    export HOME=$(mktemp -d -t --suffix .prowhome)
fi
# In some cases we install tools via cargo, ensure that's in PATH
export PATH="$HOME/.cargo/bin:$PATH"

pkg_upgrade() {
    echo "Running dnf -y distro-sync... $(date)"
    dnf -y distro-sync
    echo "Done dnf -y distro-sync! $(date)"
}

make() {
    /usr/bin/make -j ${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN)} "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc "$@"
    make V=1
}

pkg_install() {
    echo "Running dnf -y install... $(date)"
    dnf -y install "$@"
    echo "Done running dnf -y install! $(date)"
}

pkg_builddep() {
    echo "Running builddep... $(date)"
    # This is sadly the only case where it's a different command
    if test -x /usr/bin/dnf; then
        dnf builddep -y "$@"
    else
        yum-builddep -y "$@"
    fi
    echo "Done running builddep! $(date)"
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
      rpm -e "$@"
    fi
}
