#!/usr/bin/bash
#
# Builds libsmartcols-devel which libdnf depends on
#
# RHEL and -devel packages are just broken.  Today RHEL9 doesn't ship libsmartcols-devel,
# and it's not even in CRB so we need to rebuild it ourself.  Which, should actually be
# a normal and natural thing to do, except our RPM build process is totally
# not designed to handle chained builds sanely.  See also https://github.com/projectatomic/rpmdistro-gitoverlay/
#
# If we had a more NixOS like model where the binaries are just a *cache of the source*,
# then here we'd just add the centos "CRB" binary cache.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

if test -f /usr/lib/os-release; then
    . /usr/lib/os-release
    if [[ "${ID_LIKE}" =~ rhel ]] && [[ ${VERSION_ID} -gt 8 ]]; then
        yum -y install yum-utils
        # https://docs.fedoraproject.org/en-US/epel/#_el9
        yum config-manager --set-enabled crb
        yum -y install epel-release
        yum -y install git
        test -d util-linux || git clone https://gitlab.com/redhat/centos-stream/rpms/util-linux
        cd util-linux
        yum -y install centpkg
        yum -y builddep *.spec
        builddir=$(arch)
        if test '!' -d "$builddir"; then
            centpkg local
        fi
        rm -vf $builddir/*debuginfo*.rpm
        rm -vf $builddir/*python*.rpm
        yum -y localinstall $builddir/*.rpm
    fi
else
    echo "Unhandled OS" 1>&2
    exit 1
fi
