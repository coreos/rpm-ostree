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

  export VM=${VM:-vmcheck}
  local sshopts="-o User=root \
                 -o ControlMaster=auto \
                 -o ControlPath=/var/tmp/ssh-$VM-$(date +%s%N).sock \
                 -o ControlPersist=yes"

  # If we're provided with an ssh-config, make sure we tell
  # ssh to pick it up.
  if [ -f "${topsrcdir}/ssh-config" ]; then
    sshopts="$sshopts -F ${topsrcdir}/ssh-config"
  fi

  export SSH="ssh $sshopts $VM"
  export SCP="scp $sshopts"
}

# rsync wrapper that sets up authentication
vm_raw_rsync() {
  local rsyncopts="ssh -o User=root"
  if [ -f ${topsrcdir}/ssh-config ]; then
    rsyncopts="$rsyncopts -F '${topsrcdir}/ssh-config'"
  fi
  rsync -az --no-owner --no-group -e "$rsyncopts" "$@"
}

vm_rsync() {
  if ! test -f .vagrant/using_sshfs; then
    pushd ${topsrcdir}
    vm_raw_rsync --exclude .git/ . $VM:/var/roothome/sync
    popd
  fi
}

# run command in vm as user
# - $1    username
# - $@    command to run
vm_cmd_as() {
  local user=$1; shift
  # don't reuse root's ControlPath
  local sshopts="-o User=$user"
  if [ -f "${topsrcdir}/ssh-config" ]; then
    sshopts="$sshopts -F ${topsrcdir}/ssh-config"
  fi
  ssh $sshopts $VM "$@"
}

# run command in vm
# - $@    command to run
vm_cmd() {
  $SSH "$@"
}

# Copy argument (usually shell script) to VM, execute it there
vm_cmdfile() {
    local bin=$1
    chmod a+x ${bin}
    local bn=$(basename ${bin})
    $SCP $1 $VM:/root/${bn}
    $SSH /root/${bn}
}


# Delete anything which we might change between runs
vm_clean_caches() {
    vm_cmd rm /ostree/repo/extensions/rpmostree/pkgcache/refs/heads/* -rf
}

# run rpm-ostree in vm
# - $@    args
vm_rpmostree() {
    vm_cmd env ASAN_OPTIONS=detect_leaks=false rpm-ostree "$@"
}

# copy files to a directory in the vm
# - $1    target directory
# - $2..  files & dirs to copy
vm_send() {
  local dir=$1; shift
  vm_cmd mkdir -p $dir
  $SCP -r "$@" $VM:$dir
}

# copy the test repo to the vm
# $1  - repo file mode: nogpgcheck (default), gpgcheck, skip (don't send)
vm_send_test_repo() {
  mode=${1:-nogpgcheck}
  # note we use -c here because we might be called twice within a second
  vm_raw_rsync -c --delete ${test_tmpdir}/yumrepo $VM:/tmp/vmcheck

  if [[ $mode == skip ]]; then
    return
  fi

  cat > vmcheck.repo << EOF
[test-repo]
name=test-repo
baseurl=file:///tmp/vmcheck/yumrepo
EOF

  if [[ $mode == gpgcheck ]]; then
      cat >> vmcheck.repo <<EOF
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-25-primary
EOF
  else
      assert_streq "$mode" nogpgcheck
      echo "Enabling vmcheck.repo without GPG"
      echo 'gpgcheck=0' >> vmcheck.repo
  fi

  vm_send /etc/yum.repos.d vmcheck.repo
}

# wait until ssh is available on the vm
# - $1    timeout in second (optional)
# - $2    previous bootid (optional)
vm_ssh_wait() {
  local timeout=${1:-0}; shift
  local old_bootid=${1:-}; shift
  if ! vm_cmd true; then
     echo "Failed to log into VM, retrying with debug:"
     $SSH -o LogLevel=debug true || true
  fi
  while [ $timeout -gt 0 ]; do
    if bootid=$(vm_get_boot_id 2>/dev/null); then
        if [[ $bootid != $old_bootid ]]; then
            # if this is a reboot, display some info about new boot
            if [ -n "$old_bootid" ]; then
              vm_rpmostree status
              vm_rpmostree --version
            fi
            return 0
        fi
    fi
    if test $(($timeout % 5)) == 0; then
        echo "Still failed to log into VM, retrying for $timeout seconds"
    fi
    timeout=$((timeout - 1))
    sleep 1
  done
  false "Timed out while waiting for SSH."
}

vm_get_boot_id() {
  vm_cmd cat /proc/sys/kernel/random/boot_id
}

# Run a command in the VM that will cause a reboot
vm_reboot_cmd() {
    vm_cmd sync
    local bootid=$(vm_get_boot_id 2>/dev/null)
    vm_cmd $@ || :
    vm_ssh_wait 120 $bootid
}

# reboot the vm
vm_reboot() {
  vm_reboot_cmd systemctl reboot
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

# retrieve info from a deployment
# - $1   index of deployment (or -1 for booted)
# - $2   key to retrieve
vm_get_deployment_info() {
  local idx=$1
  local key=$2
  vm_rpmostree status --json | \
    python -c "
import sys, json
deployments = json.load(sys.stdin)[\"deployments\"]
idx = $idx
if idx < 0:
  for i, depl in enumerate(deployments):
    if depl[\"booted\"]:
      idx = i
if idx < 0:
  print \"Failed to determine currently booted deployment\"
  exit(1)
if idx >= len(deployments):
  print \"Deployment index $idx is out of range\"
  exit(1)
depl = deployments[idx]
if \"$key\" in depl:
  data = depl[\"$key\"]
  if type(data) is list:
    print \" \".join(data)
  else:
    print data
"
}

# retrieve the deployment root
# - $1   index of deployment
vm_get_deployment_root() {
  local idx=$1
  local csum=$(vm_get_deployment_info $idx checksum)
  local serial=$(vm_get_deployment_info $idx serial)
  local osname=$(vm_get_deployment_info $idx osname)
  echo /ostree/deploy/$osname/deploy/$csum.$serial
}

# retrieve info from the booted deployment
# - $1   key to retrieve
vm_get_booted_deployment_info() {
  vm_get_deployment_info -1 $1
}

# print the layered packages
vm_get_layered_packages() {
  vm_get_booted_deployment_info packages
}

# print the requested packages
vm_get_requested_packages() {
  vm_get_booted_deployment_info requested-packages
}

vm_get_local_packages() {
  vm_get_booted_deployment_info requested-local-packages
}

# check that the packages are currently layered
# - $@    packages to check for
vm_has_layered_packages() {
  local pkgs=$(vm_get_layered_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

# check that the packages are currently requested
# - $@    packages to check for
vm_has_requested_packages() {
  local pkgs=$(vm_get_requested_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

vm_has_local_packages() {
  local pkgs=$(vm_get_local_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

vm_has_dormant_packages() {
  vm_has_requested_packages "$@" && \
    ! vm_has_layered_packages "$@"
}

# retrieve the checksum of the currently booted deployment
vm_get_booted_csum() {
  vm_get_booted_deployment_info checksum
}

# make multiple consistency checks on a test pkg
# - $1    package to check for
# - $2    either "present" or "absent"
vm_assert_layered_pkg() {
  local pkg=$1; shift
  local policy=$1; shift

  set +e
  vm_has_packages $pkg;         pkg_in_rpmdb=$?
  vm_has_layered_packages $pkg; pkg_is_layered=$?
  vm_has_local_packages $pkg;   pkg_is_layered_local=$?
  vm_has_requested_packages $pkg; pkg_is_requested=$?
  [ $pkg_in_rpmdb == 0 ] && \
  ( ( [ $pkg_is_layered == 0 ] &&
      [ $pkg_is_requested == 0 ] ) ||
    [ $pkg_is_layered_local == 0 ] ); pkg_present=$?
  [ $pkg_in_rpmdb != 0 ] && \
  [ $pkg_is_layered != 0 ] && \
  [ $pkg_is_layered_local != 0 ] && \
  [ $pkg_is_requested != 0 ]; pkg_absent=$?
  set -e

  if [ $policy == present ] && [ $pkg_present != 0 ]; then
    vm_cmd rpm-ostree status
    assert_not_reached "pkg $pkg is not present"
  fi

  if [ $policy == absent ] && [ $pkg_absent != 0 ]; then
    vm_cmd rpm-ostree status
    assert_not_reached "pkg $pkg is not absent"
  fi
}

vm_assert_status_jq() {
    vm_rpmostree status --json > status.json
    assert_status_file_jq status.json "$@"
}

# Like build_rpm, but also sends it to the VM
vm_build_rpm() {
    build_rpm "$@"
    vm_send_test_repo
}

# Like vm_build_rpm but takes a yumrepo mode
vm_build_rpm_repo_mode() {
    mode=$1; shift
    build_rpm "$@"
    vm_send_test_repo $mode
}

vm_build_selinux_rpm() {
    build_selinux_rpm "$@"
    vm_send_test_repo
}

vm_get_journal_cursor() {
  vm_cmd journalctl -o json -n 1 | jq -r '.["__CURSOR"]'
}

# Wait for a message logged after $cursor matching a regexp to appear
vm_wait_content_after_cursor() {
    from_cursor=$1; shift
    regex=$1; shift
    cat > wait.sh <<EOF
#!/usr/bin/bash
set -xeuo pipefail
tmpf=\$(mktemp /var/tmp/journal.XXXXXX)
for x in \$(seq 60); do
  journalctl -u rpm-ostreed --after-cursor "${from_cursor}" > \${tmpf}
  if grep -q -e "${regex}" \${tmpf}; then
    exit 0
  else
    cat \${tmpf}
    sleep 1
  fi
done
echo "timed out after 60s" 1>&2
journalctl -u rpm-ostreed --after-cursor "${from_cursor}" | tail -100
exit 1
EOF
    vm_cmdfile wait.sh
}

vm_assert_journal_has_content() {
  from_cursor=$1; shift
  # add an extra helping of quotes for hungry ssh
  vm_cmd journalctl --after-cursor "'$from_cursor'" > tmp-journal.txt
  assert_file_has_content tmp-journal.txt "$@"
  rm -f tmp-journal.txt
}
