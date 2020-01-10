#!/bin/bash
#
# Copyright (C) 2017 Jonathan Lebon <jlebon@redhat.com>
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

# SUMMARY: check that `db` commands work correctly. Right now, we're only
# testing `db diff`.

YUMREPO=/var/tmp/vmcheck/yumrepo/packages/x86_64

check_diff() {
  arg1=$1; shift
  arg2=$1; shift
  cmd="vm_rpmostree db diff --format=diff"
  if [ -n "$arg1" ]; then
    cmd="$cmd $arg1"
  fi
  if [ -n "$arg2" ]; then
    cmd="$cmd $arg2"
  fi
  $cmd > diff.txt
  assert_file_has_content diff.txt "$@"
}

check_not_diff() {
  from=$1; shift
  to=$1; shift
  vm_rpmostree db diff --format=diff $from $to > diff.txt
  assert_not_file_has_content diff.txt "$@"
}

# make the package name start with zzz so it's last to be processed
vm_build_rpm zzz-pkg-to-downgrade version 2.0
vm_build_rpm pkg-to-remove
vm_build_rpm pkg-to-replace
vm_build_rpm pkg-to-replace-archtrans arch noarch
vm_rpmostree install pkg-to-remove pkg-to-replace pkg-to-replace-archtrans zzz-pkg-to-downgrade

# we should be able to see the diff in status now
vm_rpmostree status > status.txt
assert_file_has_content status.txt "Diff: 4 added"
vm_rpmostree status -v > statusv.txt
assert_file_has_content statusv.txt \
  "Added:" \
  "zzz-pkg-to-downgrade" \
  "pkg-to-remove" \
  "pkg-to-replace" \
  "pkg-to-replace-archtrans"
echo "ok db diff in status"

booted_csum=$(vm_get_booted_csum)
pending_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_csum \
  +zzz-pkg-to-downgrade \
  +pkg-to-remove \
  +pkg-to-replace \
  +pkg-to-replace-archtrans

# also check the diff using json
vm_rpmostree db diff --format=json $booted_csum $pending_csum > diff.json
# See assert_replaced_local_pkg() for some syntax explanation. The .[1] == 0 bit
# is what filters by "pkgs added".
assert_jq diff.json \
  '[.pkgdiff|map(select(.[1] == 0))[][0]]|index("zzz-pkg-to-downgrade") >= 0' \
  '[.pkgdiff|map(select(.[1] == 0))[][0]]|index("pkg-to-remove") >= 0' \
  '[.pkgdiff|map(select(.[1] == 0))[][0]]|index("pkg-to-replace") >= 0' \
  '[.pkgdiff|map(select(.[1] == 0))[][0]]|index("pkg-to-replace-archtrans") >= 0'

# check that it's the default behaviour without both args
check_diff "" "" \
  +zzz-pkg-to-downgrade \
  +pkg-to-remove \
  +pkg-to-replace \
  +pkg-to-replace-archtrans

# check that it's the default behaviour without one arg
check_diff "$pending_csum" "" \
  +zzz-pkg-to-downgrade \
  +pkg-to-remove \
  +pkg-to-replace \
  +pkg-to-replace-archtrans

# check that diff'ing with --base yields 0 diffs
check_not_diff "--base" "" pkg-to-

# now let's make the pending csum become an update
vm_ostree_commit_layered_as_base $pending_csum vmcheck
vm_rpmostree cleanup -p
vm_rpmostree upgrade
pending_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_csum \
  +zzz-pkg-to-downgrade \
  +pkg-to-remove \
  +pkg-to-replace \
  +pkg-to-replace-archtrans
echo "ok setup"

vm_rpmostree override remove pkg-to-remove
vm_build_rpm zzz-pkg-to-downgrade version 1.0
vm_build_rpm pkg-to-replace version 2.0
vm_build_rpm pkg-to-replace-archtrans version 2.0
vm_rpmostree override replace \
  $YUMREPO/zzz-pkg-to-downgrade-1.0-1.x86_64.rpm \
  $YUMREPO/pkg-to-replace-2.0-1.x86_64.rpm \
  $YUMREPO/pkg-to-replace-archtrans-2.0-1.x86_64.rpm
vm_build_rpm pkg-to-overlay build 'echo same > pkg-to-overlay'
# some multilib handling tests (override default /bin script to skip conflicts)
vm_build_rpm pkg-to-overlay build 'echo same > pkg-to-overlay' arch i686
vm_build_rpm glibc arch i686
vm_rpmostree install pkg-to-overlay.{x86_64,i686} glibc.i686
pending_layered_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_layered_csum \
  +pkg-to-overlay-1.0-1.x86_64 \
  +pkg-to-overlay-1.0-1.i686 \
  +glibc-1.0-1.i686 \
  +pkg-to-replace-2.0 \
  +zzz-pkg-to-downgrade-1.0 \
  +pkg-to-replace-archtrans-2.0
# check that regular glibc is *not* in the list of modified/dropped packages
check_not_diff $booted_csum $pending_layered_csum \
  =glibc \
  !glibc \
  -glibc \
  =pkg-to-overlay \
  !pkg-to-overlay \
  -pkg-to-overlay
check_diff $pending_csum $pending_layered_csum \
  +pkg-to-overlay-1.0-1.x86_64 \
  +pkg-to-overlay-1.0-1.i686 \
  +glibc-1.0-1.i686 \
  -pkg-to-remove \
  !zzz-pkg-to-downgrade-2.0 \
  =zzz-pkg-to-downgrade-1.0 \
  !pkg-to-replace-1.0 \
  =pkg-to-replace-2.0 \
  !pkg-to-replace-archtrans-1.0 \
  =pkg-to-replace-archtrans-2.0
# also do a human diff check
vm_rpmostree db diff $pending_csum $pending_layered_csum > diff.txt
assert_file_has_content diff.txt 'Upgraded'
assert_file_has_content diff.txt 'Downgraded'
assert_file_has_content diff.txt 'Removed'
assert_file_has_content diff.txt 'Added'
grep -A1 '^Downgraded:' diff.txt | grep zzz-pkg-to-downgrade
echo "ok db diff"

# this is a bit convoluted; basically, we prune the commit and only keep its
# metadata to check that `db diff` is indeed using the rpmdb.pkglist metadata
commit_path=$(get_obj_path /ostree/repo $pending_layered_csum commit)
vm_cmd test -f $commit_path
vm_cmd cp $commit_path $commit_path.bak
vm_rpmostree cleanup -p
vm_cmd test ! -f $commit_path
vm_cmd mv $commit_path.bak $commit_path
if vm_cmd ostree checkout --subpath /usr/share/rpm $pending_layered_csum; then
  assert_not_reached "Was able to checkout /usr/share/rpm?"
fi
check_diff $pending_csum $pending_layered_csum \
  +pkg-to-overlay-1.0-1.x86_64 \
  +pkg-to-overlay-1.0-1.i686 \
  +glibc-1.0-1.i686 \
  -pkg-to-remove \
  !pkg-to-replace-1.0 \
  =pkg-to-replace-2.0 \
  !pkg-to-replace-archtrans-1.0 \
  =pkg-to-replace-archtrans-2.0
echo "ok db from pkglist.metadata"

# check that db list also works fine from pkglist.metadata
vm_rpmostree db list $pending_layered_csum > out.txt
assert_not_file_has_content out.txt \
  pkg-to-remove \
  pkg-to-replace-1.0 \
  pkg-to-replace-archtrans-1.0
assert_file_has_content out.txt \
  pkg-to-overlay-1.0-1.x86_64 \
  pkg-to-overlay-1.0-1.i686 \
  glibc-1.0-1.i686 \
  pkg-to-replace-2.0 \
  pkg-to-replace-archtrans-2.0
echo "ok list from pkglist.metadata"
