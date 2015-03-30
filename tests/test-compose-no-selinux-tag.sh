#!/bin/bash
#
# Copyright (C) 2015 Red Hat Inc.
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

set -e

. $(dirname $0)/libtest.sh

check_root_test

# Workaround a debugging message "Missing callback called fullpath" that let the test fails.
# Remove once it doesn't happen anymore.
unset G_DEBUG

(arch | grep -q x86_64) || { echo 1>&2 "$0 can be run only on x86_64"; echo "1..0" ; exit 77; }

echo "1..1"

ostree init --repo=repo --mode=archive-z2

echo "ok setup"

rpm-ostree --repo=repo compose tree ${SRCDIR}/test-repo-no-selinux-tag.json
