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

# prepares the VM and library for action
vm_setup() {

  # We assume that there's already a configured ssh-config
  # file available to tell us how to connect to the VM.
  if [ ! -f "${topsrcdir}/ssh-config" ]; then
    echo "ERROR: No ssh-config found."
    exit 1
  fi

  local sshopts="-F ${topsrcdir}/ssh-config \
                 -o User=root \
                 -o ControlMaster=auto \
                 -o ControlPath=${topsrcdir}/ssh.sock \
                 -o ControlPersist=yes"
  export SSH="ssh $sshopts vmcheck"
  export SCP="scp $sshopts"
}

vm_rsync() {
  if ! test -f .vagrant/using_sshfs; then
    pushd ${topsrcdir}
    rsync -az --no-owner --no-group -e "ssh -F ssh-config" \
              --exclude .git/ . vmcheck:/var/roothome/sync
    popd
  fi
}

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

# copy the test repo to the vm
vm_send_test_repo() {
  vm_cmd rm -rf /tmp/vmcheck
  vm_send /tmp/vmcheck ${commondir}/compose/yum/repo

  cat > vmcheck.repo << EOF
[test-repo]
name=test-repo
baseurl=file:///tmp/vmcheck/repo
EOF

  vm_send /etc/yum.repos.d vmcheck.repo
}

# wait until ssh is available on the vm
# - $1    timeout in second (optional)
# - $2    previous bootid (optional)
vm_ssh_wait() {
  timeout=${1:-0}; shift
  old_bootid=${1:-}; shift
  while [ $timeout -gt 0 ]; do
    if bootid=$(vm_get_boot_id 2>/dev/null); then
        if [[ $bootid != $old_bootid ]]; then
            return 0
        fi
    fi
    timeout=$((timeout - 1))
    sleep 1
  done
  false "Timed out while waiting for SSH."
}

vm_get_boot_id() {
  vm_cmd cat /proc/sys/kernel/random/boot_id 2>/dev/null
}

# reboot the vm
vm_reboot() {
  vm_cmd sync
  bootid=$(vm_get_boot_id)
  vm_cmd systemctl reboot || :
  vm_ssh_wait 120 $bootid
}

# check that the given files/dirs exist on the VM
# - $@    files/dirs to check for
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

# retrieve info from the booted deployment
# - $1   key to retrieve
vm_get_booted_deployment_info() {
  key=$1
  vm_cmd rpm-ostree status --json | \
    python -c "
import sys, json
deployments = json.load(sys.stdin)[\"deployments\"]
booted = None
for deployment in deployments:
  if deployment[\"booted\"]:
    booted = deployment
    break
if not booted:
  print \"Failed to determine currently booted deployment\"
  exit(1)
if \"$key\" in booted:
  data = booted[\"$key\"]
  if type(data) is list:
    print \" \".join(data)
  else:
    print data
"
}

# print the layered packages
vm_get_layered_packages() {
  vm_get_booted_deployment_info packages
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

# retrieve the checksum of the currently booted deployment
vm_get_booted_csum() {
  vm_get_booted_deployment_info checksum
}

# make multiple consistency checks on a test pkg
# - $1    package to check for
# - $2    either "present" or "absent"
vm_assert_layered_pkg() {
  pkg=$1; shift
  policy=$1; shift

  set +e
  vm_has_packages $pkg;         pkg_in_rpmdb=$?
  vm_has_layered_packages $pkg; pkg_is_layered=$?
  [ $pkg_in_rpmdb == 0 ] && [ $pkg_is_layered == 0 ]; pkg_present=$?
  [ $pkg_in_rpmdb != 0 ] && [ $pkg_is_layered != 0 ]; pkg_absent=$?
  set -e

  if [ $policy == present ] && [ $pkg_present != 0 ]; then
    assert_not_reached "pkg $pkg is not present"
  fi

  if [ $policy == absent ] && [ $pkg_absent != 0 ]; then
    assert_not_reached "pkg $pkg is not absent"
  fi
}
