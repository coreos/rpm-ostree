#!/usr/bin/bash

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
    dnf -y install dnf-plugins-core
    dnf install -y @buildsys-build
    dnf install -y 'dnf-command(builddep)'

    # builddeps+runtime deps
    dnf builddep -y $pkg
    dnf install -y polkit-devel
    dnf install -y $pkg
    rpm -e $pkg
}
