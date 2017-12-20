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

YUMREPO=/tmp/vmcheck/yumrepo/packages/x86_64

check_diff() {
  from=$1; shift
  to=$1; shift
  vm_rpmostree db diff --format=diff $from $to > diff.txt
  assert_file_has_content diff.txt "$@"
}

vm_build_rpm pkg-to-remove
vm_build_rpm pkg-to-replace
vm_rpmostree install pkg-to-remove pkg-to-replace

booted_csum=$(vm_get_booted_csum)
pending_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_csum \
  +pkg-to-remove \
  +pkg-to-replace

# now let's make the pending csum become an update
vm_cmd ostree commit -b vmcheck --tree=ref=$pending_csum
vm_rpmostree cleanup -p
vm_rpmostree upgrade
pending_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_csum \
  +pkg-to-remove \
  +pkg-to-replace
echo "ok setup"

vm_rpmostree override remove pkg-to-remove
vm_build_rpm pkg-to-replace version 2.0
vm_rpmostree override replace $YUMREPO/pkg-to-replace-2.0-1.x86_64.rpm
vm_build_rpm pkg-to-overlay
vm_rpmostree install pkg-to-overlay
pending_layered_csum=$(vm_get_pending_csum)
check_diff $booted_csum $pending_layered_csum \
  +pkg-to-overlay \
  +pkg-to-replace-2.0
check_diff $pending_csum $pending_layered_csum \
  +pkg-to-overlay \
  -pkg-to-remove \
  !pkg-to-replace-1.0 \
  =pkg-to-replace-2.0
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
  +pkg-to-overlay \
  -pkg-to-remove \
  !pkg-to-replace-1.0 \
  =pkg-to-replace-2.0
echo "ok db from pkglist.metadata"
