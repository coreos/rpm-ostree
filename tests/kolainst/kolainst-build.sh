#!/bin/bash
# This pre-generates RPMs for testing that will be provided
# to the kola tests as data/, so we don't need to rpmbuild.
set -euo pipefail
dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd $dn/../.. && pwd)
commondir=$(cd "$dn/../common" && pwd)
export topsrcdir commondir
. "${commondir}/libtest.sh"

rm rpm-repos -rf

test_tmpdir=$(mktemp -d)
mkdir ${test_tmpdir}/rpm-repos
repover=0

# Right now we build a few RPMs, with one repo version,
# but the idea is to extend this with more.
mkdir ${test_tmpdir}/rpm-repos/${repover}
# The obligatory `foo` and `bar` packages
build_rpm foo version 1.2 release 3
build_rpm bar
build_rpm baz
build_rpm baz version 2.0
build_rpm boo
build_rpm boo version 2.0
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
             summary "awesome-daemon-for-testing" \
             install "mkdir -p %{buildroot}/{usr/bin,var/lib/%{name}}
                      install %{name} %{buildroot}/usr/bin" \
             provides "provided-testing-daemon" \
             pre "getent group testdaemon-group &>/dev/null || groupadd -r testdaemon-group
                  getent passwd testdaemon-user &>/dev/null || useradd -r testdaemon-user -g testdaemon-group -s /sbin/nologin" \
             files "/usr/bin/%{name}
                    /var/lib/%{name}"
# Will be useful for testing cancellation
build_rpm testpkg-post-infinite-loop \
             post "echo entering testpkg-post-infinite-loop 1>&2; while true; do sleep 1h; done"
build_rpm testpkg-touch-run \
             post "touch /{run,tmp,var/tmp}/should-not-persist"

# Will be useful in checking difference in package version while doing apply-live 
build_rpm pkgsystemd \
  version 1.0 \
  release 1 \
  build 'echo -e "[Unit]\nDescription=Testinglive apply\n\n[Service]\nExecStart=/bin/bash echo done\n\n[Install]\nWantedBy=multi-user.target" > %{name}.service' \
  install "mkdir -p %{buildroot}/usr/lib/systemd/system
           install %{name}.service %{buildroot}/usr/lib/systemd/system" \
  files "/usr/lib/systemd/system/%{name}.service"

# Test module
build_rpm foomodular
build_module foomodular \
  stream no-profile \
  rpm foomodular-0:1.0-1.x86_64
build_module foomodular \
  stream no-default-profile \
  profile myprof:foomodular \
  profile myotherprof:foomodular \
  rpm foomodular-0:1.0-1.x86_64
build_module foomodular \
  stream with-default-profile \
  profile default:foomodular \
  profile myotherprof:foomodular \
  rpm foomodular-0:1.0-1.x86_64
build_module_defaults foomodular \
  defprofile with-default-profile:default

# To test remote override replace
build_rpm zincati version 99.99 release 2
build_rpm zincati version 99.99 release 3

mv ${test_tmpdir}/yumrepo/* ${test_tmpdir}/rpm-repos/${repover}

# To test remote override replace update
repover=1
mkdir ${test_tmpdir}/rpm-repos/${repover}
build_rpm zincati version 99.99 release 4

#different package version to test apply-live
build_rpm pkgsystemd \
  version 2.0 \
  release 1 \
  build 'echo -e "[Unit]\nDescription=Testinglive apply\n\n[Service]\nExecStart=/bin/bash echo upgradeDone\n\n[Install]\nWantedBy=multi-user.target" > %{name}.service' \
  install "mkdir -p %{buildroot}/usr/lib/systemd/system 
           install %{name}.service %{buildroot}/usr/lib/systemd/system" \
  files "/usr/lib/systemd/system/%{name}.service"
mv ${test_tmpdir}/yumrepo/* ${test_tmpdir}/rpm-repos/${repover}

# Create an empty repo when we want to test inability to find a package
repover=2
mkdir ${test_tmpdir}/rpm-repos/${repover}
(cd ${test_tmpdir}/rpm-repos/${repover} && createrepo_c --no-database .)

# Other repo versions here e.g.
# repover=3
# mkdir ${test_tmpdir}/rpm-repos/${repover}
# ...
# mv ${test_tmpdir}/yumrepo/* ${test_tmpdir}/rpm-repos/${repover}

# And finally; put in place. This is the marker for `make` to know we succeeded.
mv ${test_tmpdir}/rpm-repos rpm-repos
