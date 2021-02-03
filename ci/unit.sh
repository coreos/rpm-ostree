#!/bin/bash
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

ci/installdeps.sh
ci/install-extra-builddeps.sh
ci/build.sh
