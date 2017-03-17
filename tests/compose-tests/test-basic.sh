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

ostree --repo=${repobuild} ls -R ${treeref} /usr/lib/ostree-boot > bootls.txt
assert_file_has_content bootls.txt vmlinuz
assert_file_has_content bootls.txt initramfs
echo "ok boot files"

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
echo "ok manpages"
