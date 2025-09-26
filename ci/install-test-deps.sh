#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -euo pipefail
dn=$(dirname $0)
. ${dn}/libbuild.sh

# Enable EPEL for packages not available in base repos
if [ -f /usr/lib/os-release ]; then
    . /usr/lib/os-release
    if [[ "${ID_LIKE:-}" =~ rhel ]] && [[ "${VERSION_ID%%.*}" -gt 8 ]]; then
        echo "Enabling EPEL repository for RHEL-like system"
        # Install yum-utils first for dnf config-manager
        pkg_install yum-utils
        # Enable CRB repository (required for many EPEL packages)
        dnf config-manager --set-enabled crb
        # Now install EPEL
        pkg_install epel-release
    fi
fi

deps=$(grep -v '^#' "${dn}"/testdeps.txt)
pkg_install ${deps}
