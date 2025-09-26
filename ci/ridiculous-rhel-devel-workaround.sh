#!/usr/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
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
        yum -y install rpm-build rpmdevtools wget
        yum -y builddep *.spec
        
        # Download the source tarball since CentOS repos don't include it
        if test -f sources; then
            source_file=$(awk '{print $2}' sources | tr -d '()')
            if test ! -f "$source_file"; then
                echo "Downloading $source_file..."
                # Extract version (e.g., "2.40" from "util-linux-2.40.2.tar.xz")
                version=$(echo "$source_file" | sed 's/util-linux-\([0-9]*\.[0-9]*\).*/\1/')
                # Util-linux releases are available from kernel.org
                wget "https://www.kernel.org/pub/linux/utils/util-linux/v${version}/${source_file}"
                # Verify the downloaded file matches the expected checksum
                sha512sum -c sources
            fi
        fi
        
        builddir=$(arch)
        if test '!' -d "$builddir"; then
            # Build directly with rpmbuild instead of centpkg to avoid krb5 auth issues
            mkdir -p "$builddir"
            rpmbuild --define "_rpmdir $(pwd)" --define "_builddir $(pwd)/BUILD" \
                     --define "_sourcedir $(pwd)" --define "_specdir $(pwd)" \
                     --define "_srcrpmdir $(pwd)" -ba *.spec
            # Move built RPMs to the expected directory structure
            mv noarch/*.rpm "$builddir/" 2>/dev/null || true
            mv x86_64/*.rpm "$builddir/" 2>/dev/null || true
        fi
        rm -vf $builddir/*debuginfo*.rpm
        rm -vf $builddir/*python*.rpm
        yum -y localinstall $builddir/*.rpm
    fi
else
    echo "Unhandled OS" 1>&2
    exit 1
fi
