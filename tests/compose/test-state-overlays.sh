#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'

# An RPM that installs in /opt
build_rpm test-opt \
             install "mkdir -p %{buildroot}/opt/megacorp/bin
                      install %{name} %{buildroot}/opt/megacorp/bin" \
             files "/opt/megacorp"

# An RPM that installs in /usr/local
build_rpm test-usr-local \
             install "mkdir -p %{buildroot}/usr/local/bin
                      install %{name} %{buildroot}/usr/local/bin" \
             files "/usr/local/bin/%{name}"

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo

# the top-level manifest doesn't have any packages, so just set it
treefile_append "packages" '["test-opt", "test-usr-local"]'

# enable state overlays
treefile_set "opt-usrlocal" '"stateoverlay"'

runcompose

# shellcheck disable=SC2154
ostree --repo="${repo}" ls -R "${treeref}" /usr/lib/opt > opt.txt
assert_file_has_content opt.txt "/usr/lib/opt/megacorp/bin/test-opt"

ostree --repo="${repo}" ls -R "${treeref}" /usr/local > usr-local.txt
assert_file_has_content usr-local.txt "/usr/local/bin/test-usr-local"

ostree --repo="${repo}" ls -R "${treeref}" /usr/lib/systemd/system/local-fs.target.requires > local-fs.txt
assert_file_has_content local-fs.txt "ostree-state-overlay@usr-lib-opt.service"
assert_file_has_content local-fs.txt "ostree-state-overlay@usr-local.service"

echo "ok /opt and /usr/local RPMs"
