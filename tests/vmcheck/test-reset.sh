#!/bin/bash
#
# Copyright (C) 2018 Jonathan Lebon <jonathan@jlebon.com>
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

# add a builtin foobar
vm_build_rpm foo
vm_rpmostree install foo
vm_cmd ostree refs $(vm_get_pending_csum) --create vmcheck_tmp/with_foo
vm_rpmostree cleanup -p
vm_ostree_commit_layered_as_base vmcheck_tmp/with_foo vmcheck
vm_rpmostree upgrade

# now do some layering, overrides, and initramfs
vm_build_rpm foo version 2.0
vm_build_rpm bar
vm_build_rpm baz
vm_rpmostree override replace --install bar \
  --install /var/tmp/vmcheck/yumrepo/packages/x86_64/baz-1.0-1.x86_64.rpm \
  /var/tmp/vmcheck/yumrepo/packages/x86_64/foo-2.0-1.x86_64.rpm
vm_rpmostree initramfs --enable

vm_reboot
vm_assert_status_jq \
  '.deployments[0].booted' \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 1' \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["regenerate-initramfs"]'
echo "ok setup"

# check removing layering only
vm_rpmostree reset --overlays
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 0' \
  '.deployments[0]["requested-packages"]|length == 0' \
  '.deployments[0]["requested-local-packages"]|length == 0' \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["regenerate-initramfs"]'
vm_rpmostree cleanup -p
echo "ok reset overlays"

# check removing overrides only
vm_rpmostree reset --overrides
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 1' \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["regenerate-initramfs"]'
vm_rpmostree cleanup -p
echo "ok reset overrides"

# check stopping initramfs only
vm_rpmostree reset --initramfs
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 1' \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["regenerate-initramfs"]|not'
vm_rpmostree cleanup -p
echo "ok reset initramfs"

# all together now
vm_rpmostree reset
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 0' \
  '.deployments[0]["requested-packages"]|length == 0' \
  '.deployments[0]["requested-local-packages"]|length == 0' \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["regenerate-initramfs"]|not'
vm_rpmostree cleanup -p
echo "ok reset EVERYTHING"

# reset everything and overlay at the same time
vm_build_rpm a-new-package
vm_rpmostree reset --install a-new-package
vm_assert_status_jq \
  '.deployments[0]["packages"]|length == 1' \
  '.deployments[0]["packages"]|index("a-new-package") >= 0' \
  '.deployments[0]["requested-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 0' \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["regenerate-initramfs"]|not'
echo "ok reset --install"
