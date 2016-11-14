#!/bin/bash
set -euo pipefail

. ${commondir}/libvm.sh

set -x
cd ${builddir}
rm insttree -rf
make install DESTDIR=$(pwd)/insttree
ssh -o User=root -F ssh-config vmcheck "ostree admin unlock; cd ~vagrant/sync && rsync -rv insttree/usr/ /usr/ && systemctl restart rpm-ostreed"
