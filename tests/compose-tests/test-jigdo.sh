#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh
. ${dn}/../common/libtest.sh

prepare_compose_test "jigdo"
# Add a local rpm-md repo so we can mutate local test packages
pyappendjsonmember "repos" '["test-repo"]'
build_rpm test-pkg \
          files "/usr/bin/test-pkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo localpkg data > %{buildroot}/usr/bin/test-pkg"
# The test suite writes to pwd, but we need repos in composedata
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln yumrepo.repo composedata/test-repo.repo
pyappendjsonmember "packages" '["test-pkg"]'
# Need unified core for this, as well as a cachedir
mkdir cache
runcompose --ex-unified-core --cachedir $(pwd)/cache --add-metadata-string version=42.0
npkgs=$(rpm-ostree --repo=${repobuild} db list ${treeref} |grep -v '^ostree commit' | wc -l)
echo "npkgs=${npkgs}"
rpm-ostree --repo=${repobuild} db list ${treeref} test-pkg >test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'

rev=$(ostree --repo=${repobuild} rev-parse ${treeref})
mkdir jigdo-output
do_commit2jigdo() {
    targetrev=$1
    rpm-ostree ex commit2jigdo --repo=repo-build --pkgcache-repo cache/pkgcache-repo ${targetrev} $(pwd)/composedata/fedora-atomic-host-oirpm.spec $(pwd)/jigdo-output
    (cd jigdo-output && createrepo_c .)
}
do_commit2jigdo ${rev}
find jigdo-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'fedora-atomic-host-42.0.*x86_64'

ostree --repo=jigdo-unpack-repo init --mode=bare-user
echo 'fsync=false' >> jigdo-unpack-repo/config
# Technically this isn't part of composedata but eh
cat > composedata/jigdo-test.repo <<eof
[jigdo-test]
baseurl=file://$(pwd)/jigdo-output
enabled=1
gpgcheck=0
eof
do_jigdo2commit() {
    rpm-ostree ex jigdo2commit -d $(pwd)/composedata -e fedora-local -e test-repo -e jigdo-test --repo=jigdo-unpack-repo fedora-atomic-host | tee jigdo2commit-out.txt
}
do_jigdo2commit
# there will generally be pkgs not in the jigdo set, but let's at least assert it's > 0
assert_file_has_content jigdo2commit-out.txt '[1-9][0-9]* packages to import'
ostree --repo=jigdo-unpack-repo rev-parse ${rev}
ostree --repo=jigdo-unpack-repo fsck
ostree --repo=jigdo-unpack-repo refs > jigdo-refs.txt
assert_file_has_content jigdo-refs.txt 'rpmostree/pkg/test-pkg/1.0-1.x86__64'

echo "ok jigdo ♲📦 fresh assembly"

origrev=${rev}
unset rev
# Update test-pkg
build_rpm test-pkg \
          version 1.1 \
          files "/usr/bin/test-pkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo localpkg data 1.1 > %{buildroot}/usr/bin/test-pkg"
# Also add an entirely new package
build_rpm test-newpkg \
          files "/usr/bin/test-newpkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo new localpkg data > %{buildroot}/usr/bin/test-newpkg"
pyappendjsonmember "packages" '["test-newpkg"]'
runcompose --ex-unified-core --cachedir $(pwd)/cache --add-metadata-string version=42.1
newrev=$(ostree --repo=${repobuild} rev-parse ${treeref})
rpm-ostree --repo=${repobuild} db list ${treeref} test-newpkg >test-newpkg-list.txt
assert_file_has_content test-newpkg-list.txt 'test-newpkg-1.0-1.x86_64'

# Jigdo version 42.1
do_commit2jigdo ${newrev}
find jigdo-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'fedora-atomic-host-42.1.*x86_64'

# And pull it; we should download the newer version by default
do_jigdo2commit
# Now we should only download 2 packages
assert_file_has_content jigdo2commit-out.txt '2 packages to import'
for x in ${origrev} ${newrev}; do
    ostree --repo=jigdo-unpack-repo rev-parse ${x}
done
ostree --repo=jigdo-unpack-repo fsck
ostree --repo=jigdo-unpack-repo refs > jigdo-refs.txt
# We should have both refs; GC will be handled by the sysroot upgrader
# via deployments, same way it is for pkg layering.
assert_file_has_content jigdo-refs.txt 'rpmostree/pkg/test-pkg/1.0-1.x86__64'
assert_file_has_content jigdo-refs.txt 'rpmostree/pkg/test-pkg/1.1-1.x86__64'

echo "ok jigdo ♲📦 update!"
