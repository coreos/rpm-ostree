#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "jigdo"
# Need unified core for this, as well as a cachedir
mkdir cache
runcompose --ex-unified-core --cachedir $(pwd)/cache --add-metadata-string version=42.0

rev=$(ostree --repo=${repobuild} rev-parse ${treeref})
mkdir jigdo-output
rpm-ostree ex commit2jigdo --repo=repo-build --pkgcache-repo cache/pkgcache-repo ${rev} $(pwd)/composedata/fedora-atomic-host-oirpm.spec $(pwd)/jigdo-output
find jigdo-output -name '*.rpm' | tee rpms.txt
test -s rpms.txt
(cd jigdo-output && createrepo_c .)

ostree --repo=jigdo-unpack-repo init --mode=bare-user
echo 'fsync=false' >> jigdo-unpack-repo/config
cat > composedata/jigdo-test.repo <<EOF
[jigdo-test]
baseurl=file://$(pwd)/jigdo-output
enabled=1
gpgcheck=0
EOF
rpm-ostree ex jigdo2commit -d $(pwd)/composedata -e fedora-local -e jigdo-test --repo=jigdo-unpack-repo fedora-atomic-host
ostree --repo=jigdo-unpack-repo rev-parse ${rev}
ostree --repo=jigdo-unpack-repo fsck

echo "ok jigdo ♲📦"
