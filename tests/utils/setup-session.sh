#!/bin/bash
#
# Configure a D-Bus session for shell script tests
#
# Copyright (C) 2015 Red Hat, Inc.
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

. ${commondir}/libtest.sh

# libtest.sh should have added the builddir which contains rpm-ostreed to our
# path
exec_binary="$(which rpm-ostreed)"

mkdir -p sysroot
mkdir -p session-services

# Create a busconfig file with a custom <servicedir>
cat > session.conf <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>

  <!-- Enable to assist with debugging -->
  <!-- <syslog/> -->

  <listen>unix:tmpdir=/tmp</listen>

  <servicedir>${test_tmpdir}/session-services</servicedir>

  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

# Create a service file with a custom --sysroot
cat > session-services/rpmostree.service <<EOF
[D-BUS Service]
Name=org.projectatomic.rpmostree1
Exec=${exec_binary} --debug --sysroot=${test_tmpdir}/sysroot
EOF

export RPMOSTREE_USE_SESSION_BUS=1

# Don't flag deployments as immutable so that test harnesses can
# easily clean up.
export OSTREE_SYSROOT_DEBUG=mutable-deployments

exec dbus-run-session --config-file=${test_tmpdir}/session.conf $@
