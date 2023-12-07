#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
build_rpm prepare-root-config \
          files "/usr/lib/ostree/prepare-root.conf" \
          install "mkdir -p %{buildroot}/usr/lib/ostree && echo -e '[root]\ntransient=true' > %{buildroot}/usr/lib/ostree/prepare-root.conf"

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
# the top-level manifest doesn't have any packages, so just set it
treefile_append "packages" '["prepare-root-config"]'

# Do the compose
runcompose
echo "ok compose"

ostree --repo=${repo} ls ${treeref} /opt > ls.txt
assert_file_has_content ls.txt 'd00755  *0  *0  *0  */opt'
echo "ok opt is directory with transient rootfs"
