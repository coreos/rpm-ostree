# Source library for installed virtualized shell script tests
#
# Copyright (C) 2016 Jonathan Lebon <jlebon@redhat.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

# run command in vm
# - $@    command to run
vm_cmd() {
  $SSH "$@"
}

# copy files to a directory in the vm
# - $1    target directory
# - $2..  files & dirs to copy
vm_send() {
  dir=$1; shift
  vm_cmd mkdir -p $dir
  $SCP -r "$@" vmcheck:$dir
}

# wait until ssh is available on the vm
vm_ssh_wait() {
  # XXX: add timeout
  while ! vm_cmd true &> /dev/null; do
    sleep 1
  done
}

# reboot the vm
vm_reboot() {
  vm_cmd systemctl reboot || :
  sleep 2 # give time for port to go down
  vm_ssh_wait
}

# check that the given files exist on the VM
# - $@    packages to check for
vm_has_files() {
  for file in "$@"; do
    if ! vm_cmd test -e $file; then
        return 1
    fi
  done
}

# check that the packages are installed
# - $@    packages to check for
vm_has_packages() {
  for pkg in "$@"; do
    if ! vm_cmd rpm -q $pkg; then
        return 1
    fi
  done
}

# print the layered packages
vm_get_layered_packages() {
  vm_cmd rpm-ostree status --json | \
    python -c '
import sys, json
depl = json.load(sys.stdin)["deployments"][0]
if "packages" in depl:
  print " ".join(depl["packages"])
'
}

# check that the packages are currently layered
# - $@    packages to check for
vm_has_layered_packages() {
  pkgs=$(vm_get_layered_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}
