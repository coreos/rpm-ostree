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

vm_rpmostree install {foo,bar}-ext-{1,2}
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_bar
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_ostree_commit_layered_as_base vmcheck_tmp/with_foo_and_bar vmcheck
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
echo "ok override replace both"
