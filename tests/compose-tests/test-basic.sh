#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "basic"
# Test metadata json with objects, arrays, numbers
cat > metadata.json <<EOF
{
  "exampleos.gitrepo": {
     "rev": "97ec21c614689e533d294cdae464df607b526ab9",
     "src": "https://gitlab.com/exampleos/custom-atomic-host"
  },
  "exampleos.tests": ["smoketested", "e2e"]
}
EOF
runcompose --add-metadata-from-json metadata.json
ostree --repo=${repobuild} ls -R ${treeref} /usr/lib/ostree-boot > bootls.txt
if ostree --repo=${repobuild} ls -R ${treeref} /usr/etc/passwd-; then
    assert_not_reached "Found /usr/etc/passwd- backup file in tree"
fi
echo "ok compose"

ostree --repo=${repobuild} show --print-metadata-key exampleos.gitrepo ${treeref} > meta.txt
assert_file_has_content meta.txt 'rev.*97ec21c614689e533d294cdae464df607b526ab9'
assert_file_has_content meta.txt 'src.*https://gitlab.com/exampleos/custom-atomic-host'
ostree --repo=${repobuild} show --print-metadata-key exampleos.tests ${treeref} > meta.txt
assert_file_has_content meta.txt 'smoketested.*e2e'
echo "ok metadata"

for path in /boot /usr/lib/ostree-boot; do
    ostree --repo=${repobuild} ls -R ${treeref} ${path} > bootls.txt
    assert_file_has_content bootls.txt vmlinuz-
    assert_file_has_content bootls.txt initramfs-
    echo "ok boot files"
done
kver=$(grep /vmlinuz bootls.txt | sed -e 's,.*/vmlinuz-\(.*\)-[0-9a-e].*$,\1,')
ostree --repo=${repobuild} ls ${treeref} /usr/lib/modules/${kver}/{vmlinuz,initramfs.img} >/dev/null

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
echo "ok manpages"

# https://github.com/projectatomic/rpm-ostree/issues/669
ostree --repo=${repobuild} ls  ${treeref} /tmp > ls.txt
assert_file_has_content ls.txt 'l00777 0 0      0 /tmp -> sysroot/tmp'
echo "ok /tmp"

ostree --repo=${repobuild} ls ${treeref} /usr/share/rpm > ls.txt
assert_not_file_has_content ls.txt '__db' 'lock'
ostree --repo=${repobuild} ls -R ${treeref} /usr/etc/selinux > ls.txt
assert_not_file_has_content ls.txt 'LOCK'
echo "ok no leftover files"

ostree --repo=${repobuild} cat ${treeref} /usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf > autovar.txt
# Picked this one at random as an example of something that won't likely be
# converted to tmpfiles.d upstream.  But if it is, we can change this test.
assert_file_has_content_literal autovar.txt 'd /var/cache 0755 0 0 - -'
echo "ok autovar'
