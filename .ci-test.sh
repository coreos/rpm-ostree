#!/bin/bash
set -xeuo pipefail

# add testuser if it's not already there
if ! id -u testuser &>/dev/null; then
  sudo adduser testuser
fi

sh autogen.sh --prefix=/usr --enable-installed-tests
make
make check
sudo make install
sudo gnome-desktop-testing-runner rpm-ostree
sudo --user=testuser gnome-desktop-testing-runner rpm-ostree
