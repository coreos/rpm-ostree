#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
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

set -euo pipefail

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# check that we error out right now if not providing an RPM
if vm_rpmostree override replace foo bar baz |& tee out.txt; then
  assert_not_reached "Able to replace RPMs from repos?"
fi
assert_file_has_content out.txt "Non-local replacement overrides not implemented yet"
echo "ok error on non-local replacements"

YUMREPO=/var/tmp/vmcheck/yumrepo/packages/x86_64

vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["requested-base-local-replacements"]|length == 0'

build_rpms() {
    local ver=${1:-1}
    foodir=/usr/lib/foo-base
    br_foodir='%{buildroot}'${foodir}
    vm_build_rpm foo-base \
                 version ${ver} \
                 files "%dir ${foodir}
                        ${foodir}/foo-base-1.txt
                        ${foodir}/foo-base-2.txt" \
                 install "mkdir -p ${br_foodir} && \
                          echo foo-1-${ver} > ${br_foodir}/foo-base-1.txt && \
                          echo foo-2-${ver} > ${br_foodir}/foo-base-2.txt"
    bardir=${foodir}/bar
    br_bardir='%{buildroot}'${bardir}
    vm_build_rpm bar-base \
                 version ${ver} \
                 files "%dir ${bardir}
                        ${bardir}/bar.txt" \
                 install "mkdir -p ${br_bardir} && echo bar-${ver} > ${br_bardir}/bar.txt"
    for x in 1 2; do
        vm_build_rpm foo-ext-${x} \
                     version ${ver} \
                     requires "foo-base >= ${ver}" \
                     files "${foodir}/foo-ext-${x}-1.txt
                            ${foodir}/foo-ext-${x}-2.txt" \
                     install "mkdir -p ${br_foodir} && \
                              for y in 1 2; do echo foo-ext-${ver}-\${y} > ${br_foodir}/foo-ext-${x}-\${y}.txt; done"
        vm_build_rpm bar-ext-${x} \
                     version ${ver} \
                     requires "bar-base >= ${ver}" \
                     files "${bardir}/bar-ext-${x}-1.txt
                            ${bardir}/bar-ext-${x}-2.txt" \
                     install "mkdir -p ${br_bardir} && \
                              for y in 1 2; do echo bar-ext-${ver}-\${y} > ${br_bardir}/bar-ext-${x}-\${y}.txt; done"
    done
}
build_rpms

vm_status_watch_start
vm_rpmostree install {foo,bar}-ext-{1,2}
vm_status_watch_check "Transaction: install foo-ext-1 foo-ext-2 bar-ext-1 bar-ext-2"

# also install a pkg whose /var will change to verify that we delete our
# generated tmpfiles.d entry (https://github.com/coreos/rpm-ostree/pull/3228)
vm_build_rpm pkg-with-var \
  files "/var/pkg-with-var" \
  install "mkdir -p '%{buildroot}/var/pkg-with-var'"

vm_rpmostree install pkg-with-var

vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_bar_and_pkg_with_var
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_ostree_commit_layered_as_base vmcheck_tmp/with_foo_and_bar_and_pkg_with_var vmcheck
vm_rpmostree upgrade
vm_reboot
echo "ok setup"

# Replace *just* the base parts for
# https://github.com/projectatomic/rpm-ostree/issues/1340
ver=2
build_rpms ${ver}
prev_root=$(vm_get_deployment_root 0)
vm_cmd /bin/sh -c "cd ${prev_root} && find . -print" > prev-list.txt
vm_rpmostree override replace $YUMREPO/{foo,bar}-base-${ver}-1.x86_64.rpm
vm_assert_status_jq \
    '.deployments[0]["base-local-replacements"]|length == 2' \
    '.deployments[0]["requested-base-local-replacements"]|length == 2'
new_root=$(vm_get_deployment_root 0)
vm_cmd /bin/sh -c "cd ${new_root} && find . -print" > new-list.txt
diff -u {prev,new}-list.txt
rm -f *-list.txt
vm_rpmostree cleanup -p
echo "ok override replace base"

# Now replace both
ver=3
build_rpms ${ver}
prev_root=$(vm_get_deployment_root 0)
vm_cmd /bin/sh -c "cd ${prev_root} && find . -print" > prev-list.txt
vm_rpmostree override replace $YUMREPO/{foo,bar}-base-${ver}-1.x86_64.rpm $YUMREPO/{foo,bar}-ext-{1,2}-${ver}-1.x86_64.rpm
vm_assert_status_jq \
    '.deployments[0]["base-local-replacements"]|length == 6' \
    '.deployments[0]["requested-base-local-replacements"]|length == 6'
new_root=$(vm_get_deployment_root 0)
vm_cmd /bin/sh -c "cd ${new_root} && find . -print" > new-list.txt
diff -u {prev,new}-list.txt
rm -f *-list.txt
vm_rpmostree cleanup -p
echo "ok override replace both"

# And now verify https://github.com/coreos/rpm-ostree/pull/3228
prev_root=$(vm_get_deployment_root 0)
vm_cmd grep ' /var/pkg-with-var ' "${prev_root}/usr/lib/tmpfiles.d/pkg-pkg-with-var.conf"
vm_build_rpm pkg-with-var version 2 \
  files "/var/pkg-with-different-var" \
  install "mkdir -p '%{buildroot}/var/pkg-with-different-var'"
vm_rpmostree override replace $YUMREPO/pkg-with-var-2-1.x86_64.rpm
new_root=$(vm_get_deployment_root 0)
vm_cmd grep ' /var/pkg-with-different-var ' "${new_root}/usr/lib/tmpfiles.d/pkg-pkg-with-var.conf"
vm_rpmostree cleanup -p
echo "ok override replace deletes tmpfiles.d dropin"

# https://github.com/coreos/rpm-ostree/issues/3421
# Test that we can override selinux; we use the "gold"
# selinux because we know it won't be GC'd.  Use e.g.
# `koji latest-pkg f38 selinux-policy`
# to find this.  (In contrast, koji latest-pkg f38-updates selinux-policy
# will get the latest updates).
versionid=$(vm_cmd grep -E '^VERSION_ID=' /etc/os-release)
versionid=${versionid:11} # trim off VERSION_ID=
vm_cmd rpm-ostree db list "$(vm_get_deployment_info 0 checksum)" > current-dblist.txt
case $versionid in
  # XXX: this isn't actually the gold selinux; that one is too old for
  # container-selinux and moby-engine. rather than trying to change multiple
  # packages, we use one that's in coreos-pool since that also prevents GC
  38)
    evr=38.25-1.fc38
    koji_url='https://koji.fedoraproject.org/koji/buildinfo?buildID=2274128'
    # XXX: we need to replace container-selinux too for dep reasons
    hack='https://koji.fedoraproject.org/koji/buildinfo?buildID=2281229'
    ;;
  *) assert_not_reached "Unsupported Fedora version: $versionid";;
esac
assert_not_file_has_content current-dblist.txt selinux-policy-$evr
vm_rpmostree override replace "${koji_url}" "${hack}"
vm_rpmostree cleanup -p
echo "ok override replace selinux-policy-targeted"
