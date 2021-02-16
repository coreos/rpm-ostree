#!/bin/bash
set -euo pipefail
dn=$(dirname $0)
. ${dn}/libbuild.sh
deps=$(grep -v '^#' "${dn}"/testdeps.txt)
pkg_install ${deps}
