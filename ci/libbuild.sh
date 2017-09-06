#!/usr/bin/bash

pkg_upgrade() {
    # https://bugzilla.redhat.com/show_bug.cgi?id=1483553
    if ! yum -y upgrade 2>err.txt; then
        ecode=$?
        if grep -q -F -e "BDB1539 Build signature doesn't match environment" err.txt; then
            rpm --rebuilddb
            yum -y upgrade
        else
            cat err.txt
            exit ${ecode}
        fi
    fi
}

pkg_install() {
  yum -y install "$@"
}

make() {
    /usr/bin/make -j $(getconf _NPROCESSORS_ONLN) "$@"
}

build() {
    env NOCONFIGURE=1 ./autogen.sh
    ./configure --prefix=/usr --libdir=/usr/lib64 "$@"
    make
}

build_default() {
    export CFLAGS="${CFLAGS:-} -fsanitize=undefined"
    build
}

install_builddeps() {
    pkg=$1

    if [ -x /usr/bin/dnf ]; then
        dnf -y install dnf-plugins-core
        dnf install -y @buildsys-build
        dnf install -y 'dnf-command(builddep)'
        dnf builddep -y $pkg
    else
        yum install -y make rpm-build
        yum-builddep -y rpm-ostree
    fi

    # builddeps+runtime deps
    yum install -y $pkg
    rpm -e $pkg
}
