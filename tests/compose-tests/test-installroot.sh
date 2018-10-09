#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "installroot"
# This is used to test postprocessing with treefile vs not
pysetjsonmember "boot_location" '"new"'
instroot_tmp=$(mktemp -d /var/tmp/rpm-ostree-instroot.XXXXXX)
rpm-ostree compose install --repo="${repobuild}" ${treefile} ${instroot_tmp}
instroot=${instroot_tmp}/rootfs
assert_not_has_dir ${instroot}/usr/lib/ostree-boot
assert_not_has_dir ${instroot}/etc
test -L ${instroot}/home
assert_has_dir ${instroot}/usr/etc

# Clone the root - we'll test direct commit, as well as postprocess with
# and without treefile.
mv ${instroot}{,-postprocess}
cp -al ${instroot}{-postprocess,-directcommit}
cp -al ${instroot}{-postprocess,-postprocess-treefile}

integrationconf=usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf

assert_not_has_file ${instroot}-postprocess/${integrationconf}
rpm-ostree compose postprocess ${instroot}-postprocess
assert_has_file ${instroot}-postprocess/${integrationconf}
# Without treefile, kernels end up in "both" mode
ls ${instroot}-postprocess/boot > ls.txt
assert_file_has_content ls.txt '^vmlinuz-'
rm -f ls.txt
ostree --repo=${repobuild} commit -b test-directcommit --selinux-policy ${instroot}-postprocess --tree=dir=${instroot}-postprocess
echo "ok postprocess + direct commit"

rpm-ostree compose postprocess ${instroot}-postprocess-treefile ${treefile}
assert_has_file ${instroot}-postprocess-treefile/${integrationconf}
# with treefile, no kernels in /boot
ls ${instroot}-postprocess-treefile/boot > ls.txt
assert_not_file_has_content ls.txt '^vmlinuz-'
rm -f ls.txt
echo "ok postprocess with treefile"

testdate=$(date)
echo "${testdate}" > ${instroot}-directcommit/usr/share/rpm-ostree-composetest-split.txt
assert_not_has_file ${instroot}-directcommit/${integrationconf}
rpm-ostree compose commit --repo=${repobuild} ${treefile} ${instroot}-directcommit
ostree --repo=${repobuild} ls ${treeref} /usr/bin/bash
ostree --repo=${repobuild} cat ${treeref} /usr/share/rpm-ostree-composetest-split.txt >out.txt
assert_file_has_content_literal out.txt "${testdate}"
ostree --repo=${repobuild} cat ${treeref} /${integrationconf}
echo "ok installroot"
