#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
build_rpm test-pkg \
          files "/usr/bin/test-pkg" \
          install "mkdir -p %{buildroot}/usr/bin && echo localpkg data > %{buildroot}/usr/bin/test-pkg"

# The test suite writes to pwd, but we need repos in config
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln yumrepo.repo config/test-repo.repo
treefile_append "packages" '["test-pkg"]'
treefile_set "documentation" 'False'

runcompose --add-metadata-string version=42.0
npkgs=$(rpm-ostree --repo=${repo} db list ${treeref} |grep -v '^ostree commit' | wc -l)
echo "npkgs=${npkgs}"
rpm-ostree --repo=${repo} db list ${treeref} test-pkg >test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'

rev=$(ostree --repo=${repo} rev-parse ${treeref})
mkdir rojig-output
do_commit2rojig() {
    targetrev=$1
    echo "$(date): starting commit2rojig"
    runasroot rpm-ostree ex commit2rojig --repo=${repo} --pkgcache-repo cache/pkgcache-repo ${targetrev} ${treefile} $(pwd)/rojig-output
    (cd rojig-output && createrepo_c .)
    echo "$(date): finished commit2rojig"
}
do_commit2rojig ${rev}
test -f rojig-output/x86_64/fedora-coreos-42.0-1.fc31.x86_64.rpm

ostree --repo=rojig-unpack-repo init --mode=bare-user
echo 'fsync=false' >> rojig-unpack-repo/config
# Technically this isn't part of config/ but eh
cat > config/rojig-test.repo <<eof
[rojig-test]
baseurl=file://$(pwd)/rojig-output
enabled=1
gpgcheck=0
eof
do_rojig2commit() {
    echo "$(date): starting rojig2commit"
    rpm-ostree ex rojig2commit -d $(pwd)/config -e cache -e test-repo -e rojig-test --repo=rojig-unpack-repo rojig-test:fedora-coreos | tee rojig2commit-out.txt
    echo "$(date): finished rojig2commit"
}
do_rojig2commit
# there will generally be pkgs not in the rojig set, but let's at least assert it's > 0
assert_file_has_content rojig2commit-out.txt ${npkgs}/${npkgs}' packages to import'
ostree --repo=rojig-unpack-repo rev-parse ${rev}
echo "$(date): starting fsck"
ostree --repo=rojig-unpack-repo fsck
echo "$(date): finished fsck"
ostree --repo=rojig-unpack-repo refs > rojig-refs.txt
assert_file_has_content rojig-refs.txt 'rpmostree/rojig/test-pkg/1.0-1.x86__64'

echo "ok rojig ♲📦 fresh assembly"

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
treefile_append "packages" '["test-newpkg"]'
runcompose --add-metadata-string version=42.1
newrev=$(ostree --repo=${repo} rev-parse ${treeref})
rpm-ostree --repo=${repo} db list ${treeref} test-newpkg >test-newpkg-list.txt
assert_file_has_content test-newpkg-list.txt 'test-newpkg-1.0-1.x86_64'

# Rojig version 42.1
do_commit2rojig ${newrev}
path=rojig-output/x86_64/fedora-coreos-42.1-1.fc31.x86_64.rpm
rpm -qp --requires ${path} > requires.txt
assert_file_has_content requires.txt 'glibc(.*) = '
assert_file_has_content requires.txt 'systemd(.*) = '
assert_file_has_content requires.txt 'test-pkg(.*) = 1.1-1'

# And pull it; we should download the newer version by default
do_rojig2commit
# Now we should only download 2 packages
assert_file_has_content rojig2commit-out.txt '2/[1-9][0-9]* packages to import'
for x in ${origrev} ${newrev}; do
    ostree --repo=rojig-unpack-repo rev-parse ${x}
done
ostree --repo=rojig-unpack-repo fsck
ostree --repo=rojig-unpack-repo refs > rojig-refs.txt
# We should have both refs; GC will be handled by the sysroot upgrader
# via deployments, same way it is for pkg layering.
assert_file_has_content rojig-refs.txt 'rpmostree/rojig/test-pkg/1.0-1.x86__64'
assert_file_has_content rojig-refs.txt 'rpmostree/rojig/test-pkg/1.1-1.x86__64'

echo "ok rojig ♲📦 update!"

# Add all docs to test https://github.com/projectatomic/rpm-ostree/issues/1197
treefile_set "documentation" 'True'
runcompose --add-metadata-string version=42.2
newrev=$(ostree --repo=${repo} rev-parse ${treeref})
do_commit2rojig ${newrev}
find rojig-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'fedora-coreos-42.2.*x86_64'
do_rojig2commit
# Not every package has docs, but there are going to need to be changes
assert_file_has_content rojig2commit-out.txt '[1-9][0-9]*/[1-9][0-9]* packages to import ([1-9][0-9]* changed)'
ostree --repo=rojig-unpack-repo ls -R ${newrev} >/dev/null
echo "ok rojig ♲📦 updated docs"
