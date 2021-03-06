#!/bin/bash
# This pre-generates RPMs for testing that will be provided
# to the kola tests as data/, so we don't need to rpmbuild.
set -euo pipefail
dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(git rev-parse --show-toplevel)
commondir=$(cd "$dn/../common" && pwd)
export topsrcdir commondir
. "${commondir}/libtest.sh"

rm rpm-repos -rf
mkdir rpm-repos

test_tmpdir=$(mktemp -d)
repover=0

# Right now we build just one rpm, with one repo version,
# but the idea is to extend this with more.
mkdir rpm-repos/${repover}
# The obligatory `foo` and `bar` packages
build_rpm foo version 1.2 release 3
build_rpm bar
# And from here we lose our creativity and name things starting
# with `testpkg` and grow more content.
# This one has various files in /etc
build_rpm testpkg-etc \
  build 'echo "A config file for %{name}" > %{name}.conf' \
  install 'mkdir -p %{buildroot}/etc
           install %{name}.conf %{buildroot}/etc
           echo otherconf > %{buildroot}/etc/%{name}-other.conf
           mkdir -p %{buildroot}/etc/%{name}/
           echo subconfig-one > %{buildroot}/etc/%{name}/subconfig-one.conf
           echo subconfig-two > %{buildroot}/etc/%{name}/subconfig-two.conf
           mkdir -p %{buildroot}/etc/%{name}/subdir
           install -d %{buildroot}/etc/%{name}/subdir/subsubdir
           echo subconfig-three > %{buildroot}/etc/%{name}/subdir/subsubdir/subconfig-three.conf
           ln -s / %{buildroot}/etc/%{name}/subdir/link2root
           ln -s nosuchfile %{buildroot}/etc/%{name}/link2nowhere
           ln -s . %{buildroot}/etc/%{name}/subdir/link2self
           ln -s ../.. %{buildroot}/etc/%{name}/subdir/link2parent
           mkdir -p %{buildroot}/etc/opt
           echo file-in-opt-subdir > %{buildroot}/etc/opt/%{name}-opt.conf' \
  files "/etc/%{name}.conf
         /etc/%{name}-other.conf
         /etc/%{name}/*
         /etc/opt/%{name}*"
# A service that adds a user and has data in tmpfiles.d
build_rpm testdaemon \
             build "echo testdaemon-binary > %{name}" \
             install "mkdir -p %{buildroot}/{usr/bin,var/lib/%{name}}
                      install %{name} %{buildroot}/usr/bin" \
             pre "groupadd -r testdaemon-group
                  useradd -r testdaemon-user -g testdaemon-group -s /sbin/nologin" \
             files "/usr/bin/%{name}
                    /var/lib/%{name}"

mv ${test_tmpdir}/yumrepo/* rpm-repos/${repover}
