#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# This is used to test postprocessing with treefile vs not
treefile_set "boot-location" '"new"'

# This test is a bit of a degenerative case of the supermin abstration. We need
# to be able to interact with the compose output directly, feed it back to
# rpm-ostree, etc... So we just run whole scripts inside the VM.

case $(stat -f -c '%T' .) in
    fuseblk) fatal "This test will not work on a fuse filesystem: $(pwd)" ;;
    *) ;;
esac

instroot_tmp=cache/instroot
instroot=${instroot_tmp}/rootfs
integrationconf=usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf
runasroot sh -xec "
mkdir -p ${instroot_tmp}
rpm-ostree compose install ${compose_base_argv} ${treefile} ${instroot_tmp}

! test -d ${instroot}/etc
test -L ${instroot}/home
test -d ${instroot}/usr/etc

# Clone the root - we'll test direct commit, as well as postprocess with
# and without treefile.
mv ${instroot}{,-postprocess}
cp -al ${instroot}{-postprocess,-directcommit}
cp -al ${instroot}{-postprocess,-postprocess-treefile}


! test -f ${instroot}-postprocess/${integrationconf}
rpm-ostree compose postprocess ${instroot}-postprocess
test -f ${instroot}-postprocess/${integrationconf}
ostree --repo=${repo} commit -b test-directcommit --selinux-policy ${instroot}-postprocess --tree=dir=${instroot}-postprocess
"
echo "ok postprocess + direct commit"

runasroot sh -xec "
rpm-ostree compose postprocess ${instroot}-postprocess-treefile ${treefile}
test -f ${instroot}-postprocess-treefile/${integrationconf}
# with treefile, no kernels in /boot
ls ${instroot}-postprocess-treefile/boot > ls.txt
! grep '^vmlinuz-' ls.txt
rm -f ls.txt
"
echo "ok postprocess with treefile"

testdate=$(date)
runasroot sh -xec "
echo \"${testdate}\" > ${instroot}-directcommit/usr/share/rpm-ostree-composetest-split.txt
! test -f ${instroot}-directcommit/${integrationconf}
rpm-ostree compose commit --repo=${repo} ${treefile} ${instroot}-directcommit
ostree --repo=${repo} ls ${treeref} /usr/bin/bash
if ostree --repo=${repo} ls ${treeref} /var/lib/rpm >/dev/null; then
  echo found /var/lib/rpm in commit 1>&2; exit 1
fi
ostree --repo=${repo} cat ${treeref} /usr/share/rpm-ostree-composetest-split.txt >out.txt
grep \"${testdate}\" out.txt
ostree --repo=${repo} cat ${treeref} /${integrationconf}
"
echo "ok installroot"
