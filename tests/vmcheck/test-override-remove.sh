#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

# create a new vmcheck commit which has foo and bar in it already so that we can
# target them with our override

# make sure the packages are not already layered
vm_assert_layered_pkg foo absent
vm_assert_layered_pkg bar absent
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'

vm_cmd ostree refs $(vm_get_booted_csum) \
  --create vmcheck_tmp/without_foo_and_bar

# create a new branch with foo and bar.
# foo has files in /lib, to test our re-canonicalization to /usr
vm_build_rpm foo \
             files "%dir /lib/foo
                    /lib/foo/foo.txt
                    /lib/foo/shared.txt" \
             install 'mkdir -p %{buildroot}/lib/foo && \
                      echo %{name} > %{buildroot}/lib/foo/foo.txt && \
                      echo shared > %{buildroot}/lib/foo/shared.txt'
# make bar co-own /lib/foo and /lib/foo/shared.txt
vm_build_rpm bar \
             files "%dir /lib/foo
                    /lib/foo/bar.txt
                    /lib/foo/shared.txt" \
             install 'mkdir -p %{buildroot}/lib/foo && \
                      echo %{name} > %{buildroot}/lib/foo/bar.txt && \
                      echo shared > %{buildroot}/lib/foo/shared.txt'
vm_rpmostree install foo bar
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_bar
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_reboot
if ! vm_has_packages foo bar; then
    assert_not_reached "foo or bar not in base layer"
fi
echo "ok setup"

# And now we move the local cache; this will force an error if we ever try to
# reach the repo. This implicitly tests that `override remove/reset` operate in
# cache-only mode.
vm_rpmostree cleanup --repomd
vm_cmd mv /var/tmp/vmcheck/yumrepo{,.bak}

# funky jq syntax: see test-override-local-replace.sh for an explanation of how
# this works. the only difference here is the [.0] which we use to access the
# nevra of each gv_nevra element.

# remove just bar first to check deletion handling
vm_rpmostree override remove bar
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
newroot=$(vm_get_deployment_root 0)
# And test that we removed fully owned files, but not shared files
vm_cmd "test -d ${newroot}/usr/lib/foo && \
        test -f ${newroot}/usr/lib/foo/foo.txt && \
        test -f ${newroot}/usr/lib/foo/shared.txt && \
        test ! -f ${newroot}/usr/lib/foo/bar.txt"
echo "ok override remove bar"

# now also remove foo
vm_rpmostree override remove foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 2' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 2' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
newroot=$(vm_get_deployment_root 0)
# And this tests handling /lib -> /usr/lib as well as removal of shared files
vm_cmd "test -d ${newroot}/usr/lib && \
        test '!' -f ${newroot}/usr/lib/foo/foo.txt && \
        test '!' -f ${newroot}/usr/lib/foo/shared.txt && \
        test '!' -d ${newroot}/usr/lib/foo"
echo "ok override remove foo and bar"

vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade --cache-only
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 2' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 2' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
echo "ok override remove carried through upgrade"

vm_rpmostree override reset foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("bar-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("bar") >= 0'
echo "ok override reset foo"

vm_rpmostree override reset --all
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'
vm_rpmostree override reset --all |& tee out.txt
assert_file_has_content out.txt "No change."
echo "ok override reset --all"

# check that upgrading to a base without foo works
vm_rpmostree override remove foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo_and_bar
vm_rpmostree upgrade --cache-only
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade --cache-only
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 1' \
  '[.deployments[0]["base-removals"][][.0]]|index("foo-1.0-1.x86_64") >= 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
echo "ok active -> inactive -> active override remove"

# make sure we can reset it while it's inactive
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo_and_bar
vm_rpmostree upgrade --cache-only
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 1' \
  '.deployments[0]["requested-base-removals"]|index("foo") >= 0'
vm_rpmostree override reset foo
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'
echo "ok reset inactive override remove"

vm_rpmostree cleanup -p

# Restore the local yum repo.
vm_cmd mv /var/tmp/vmcheck/yumrepo{.bak,}
echo "ok override remove/reset operate offline"

# a few error checks

if vm_rpmostree override remove non-existent-package; then
  assert_not_reached "override remove non-existent-package succeeded?"
fi
echo "ok override remove non-existent-package fails"

vm_build_rpm baz
vm_rpmostree install baz
if vm_rpmostree override remove baz; then
  assert_not_reached "override remove layered pkg foo succeeded?"
fi
vm_rpmostree cleanup -p
echo "ok override remove layered pkg baz fails"

# the next two error checks expect an upgraded layer with foo builtin
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar

vm_rpmostree upgrade
vm_rpmostree override remove foo
if vm_rpmostree install foo; then
  assert_not_reached "tried to layer pkg removed by override"
fi
# the check blocking this isn't related to overrides, though for consistency
# let's make sure this fails here too
if vm_rpmostree install /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-1.0-1.x86_64.rpm; then
  assert_not_reached "tried to layer local pkg removed by override"
fi
vm_rpmostree cleanup -p
echo "ok can't layer pkg removed by override"

vm_build_rpm foo-ext requires "foo = 1.0-1"
vm_rpmostree upgrade --install foo-ext
if vm_rpmostree override remove foo; then
  assert_not_reached "override remove base pkg needed by layered pkg succeeded?"
fi
vm_rpmostree cleanup -p
echo "ok override remove base dep to layered pkg fails"

vm_build_rpm boo
vm_rpmostree override remove foo --install boo
vm_rpmostree cleanup -p
echo "ok remove and --install at the same time"
