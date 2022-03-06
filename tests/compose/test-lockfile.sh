#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
build_rpm test-pkg-common
build_rpm test-pkg requires test-pkg-common
build_rpm another-test-pkg-a
build_rpm another-test-pkg-b
build_rpm another-test-pkg-c

# The test suite writes to pwd, but we need repos together with the manifests
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
treefile_append "packages" '["test-pkg", "another-test-pkg-a", "another-test-pkg-b", "another-test-pkg-c"]'

runcompose --write-lockfile-to="$PWD/versions.lock"
rpm-ostree --repo=${repo} db list ${treeref} > test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-a-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-b-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-c-1.0-1.x86_64'
echo "ok compose"

assert_has_file "versions.lock"
assert_jq "versions.lock" \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg-a"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg-b"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg-c"].evra = "1.0-1.x86_64"' \
  '.metadata.rpmmd_repos|length > 0' \
  '.metadata.generated'
echo "ok lockfile created"

# Read lockfile back (should be a no-op)
build_rpm test-pkg-common version 2.0
build_rpm test-pkg version 2.0 requires test-pkg-common
build_rpm another-test-pkg-a version 2.0
build_rpm another-test-pkg-b version 2.0
build_rpm another-test-pkg-c version 2.0
runcompose --lockfile="$PWD/versions.lock" |& tee out.txt

rpm-ostree --repo=${repo} db list ${treeref} > test-pkg-list.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-a-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-b-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-c-1.0-1.x86_64'
echo "ok lockfile read"

# now add an override and check that not specifying a digest is allowed
# we test both evra and evr locking
cat > override.lock <<EOF
{
  "packages": {
    "another-test-pkg-a": {
      "evra": "2.0-1.x86_64"
    },
    "another-test-pkg-b": {
      "evr": "2.0-1"
    }
  },
  "source-packages": {
    "another-test-pkg-c": "2.0-1"
  }
}
EOF

runcompose \
  --lockfile="$PWD/versions.lock" \
  --lockfile="$PWD/override.lock" \
  --write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-a-2.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-b-2.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-c-2.0-1.x86_64'
assert_jq versions.lock.new \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg-a"].evra = "2.0-1.x86_64"' \
  '.packages["another-test-pkg-b"].evra = "2.0-1.x86_64"' \
  '.packages["another-test-pkg-c"].evra = "2.0-1.x86_64"'
echo "ok override"

# sanity-check that we can remove packages in relaxed mode
treefile_remove "packages" '"another-test-pkg-a"'
runcompose \
  --lockfile="$PWD/versions.lock" \
  --lockfile="$PWD/override.lock" \
  --write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_not_file_has_content out.txt 'another-test-pkg-a'
echo "ok relaxed mode can remove pkg"

# test strict mode

# sanity-check that refeeding the output lockfile as input satisfies strict mode
mv versions.lock{.new,}
runcompose \
  --lockfile-strict \
  --lockfile="$PWD/versions.lock" \
  --write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_streq \
  "$(jq .packages versions.lock | sha256sum)" \
  "$(jq .packages versions.lock.new | sha256sum)"
echo "ok strict mode sanity check"

# check that trying to install a pkg that's not in the lockfiles fails
build_rpm unlocked-pkg
treefile_append "packages" '["unlocked-pkg"]'
if runcompose \
    --lockfile-strict \
    --lockfile="$PWD/versions.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Packages not found: unlocked-pkg'
echo "ok strict mode no unlocked pkgs"

# check that a locked pkg with unlocked deps causes an error
build_rpm unlocked-pkg version 2.0 requires unlocked-pkg-dep
build_rpm unlocked-pkg-dep version 2.0
# notice we add unlocked-pkg, but not unlocked-pkg-dep
cat > override.lock <<EOF
{
  "packages": {
    "unlocked-pkg": {
      "evra": "2.0-1.x86_64"
    }
  }
}
EOF
if runcompose \
    --lockfile-strict \
    --lockfile="$PWD/versions.lock" \
    --lockfile="$PWD/override.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Could not depsolve transaction'
assert_file_has_content err.txt 'unlocked-pkg-dep-2.0-1.x86_64 is filtered out by exclude filtering'
treefile_remove "packages" '"unlocked-pkg"'
echo "ok strict mode no unlocked pkg deps"

# check that a locked pkg which isn't actually in the repos causes an error
cat > override.lock <<EOF
{
  "packages": {
    "unmatched-pkg": {
      "evra": "1.0-1.x86_64"
    }
  }
}
EOF
if runcompose \
    --lockfile-strict \
    --lockfile="$PWD/versions.lock" \
    --lockfile="$PWD/override.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt "Couldn't find locked package 'unmatched-pkg-1.0-1.x86_64'"
echo "ok strict mode locked pkg missing from rpmmd"

# test lockfile-repos, i.e. check that a pkg in a lockfile repo with higher
# NEVRA isn't picked unless if it's not in the lockfile

# some file shuffling to get a separate yumrepo-locked/ which has foobar-2.0
build_rpm foobar
mv yumrepo yumrepo.bak
build_rpm foobar version 2.0
mv yumrepo yumrepo-locked
mv yumrepo.bak yumrepo
sed -e 's/test-repo/test-lockfile-repo/g' < yumrepo.repo > yumrepo-locked.repo
sed -e 's/yumrepo/yumrepo-locked/g' < yumrepo-locked.repo > yumrepo-locked.repo.new
mv yumrepo-locked.repo.new yumrepo-locked.repo
ln "$PWD/yumrepo-locked.repo" config/yumrepo-locked.repo
treefile_append "packages" '["foobar"]'

# try first as a regular repo, to make sure it's functional
treefile_append "repos" '["test-lockfile-repo"]'
runcompose \
  --lockfile="$PWD/versions.lock" \
  --write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'foobar-2.0-1.x86_64'

# ok, now as a lockfile repo
treefile_remove "repos" '"test-lockfile-repo"'
treefile_append "lockfile-repos" '["test-lockfile-repo"]'
runcompose \
  --lockfile="$PWD/versions.lock" \
  --write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'foobar-1.0-1.x86_64'
treefile_remove "packages" '"foobar"'
echo "ok lockfile-repos"
