#!/usr/bin/bash

pkg_upgrade() {
    echo "Running yum -y distro-sync... $(date)"
    if ! yum -y distro-sync; then
        rc=$?
        if test -f /var/log/dnf.log; then
            grep librepo.LibrepoException /var/log/dnf.log
        fi
        exit ${rc}
    fi
    echo "Done yum -y distro-sync! $(date)"
}

make() {
    /usr/bin/make -j $(getconf _NPROCESSORS_ONLN) "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc "$@"
    make V=1
}

pkg_install() {
    echo "Running yum -y install... $(date)"
    yum -y install "$@"
    echo "Done running yum -y install! $(date)"
}

pkg_install_if_os() {
    os=$1
    shift
    (. /etc/os-release;
         if test "${os}" = "${ID}"; then
            pkg_install "$@"
         else
             echo "Skipping installation on OS ${ID}: $@"
         fi
    )
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
    pkg=$1
    if test -x /usr/bin/dnf; then
        pkg_install dnf-plugins-core 'dnf-command(builddep)'
        # Base buildroot
        pkg_install @buildsys-build
    else
        pkg_install yum-utils
        # Base buildroot, copied from the mock config sadly
        pkg_install bash bzip2 coreutils cpio diffutils system-release findutils gawk gcc gcc-c++ grep gzip info make patch redhat-rpm-config rpm-build sed shadow-utils tar unzip util-linux which xz
    fi
    # builddeps+runtime deps
    pkg_builddep $pkg
    pkg_install $pkg
    rpm -e $pkg
}
