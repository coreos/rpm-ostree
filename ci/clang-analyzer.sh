#!/usr/bin/bash
# Use the clang static analyzer

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/installdeps.sh
env NOCONFIGURE=1 ./autogen.sh
scan-build ./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
scan-build ${ARTIFACT_DIR:+-o ${ARTIFACT_DIR}} make
