#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

ci/installdeps.sh
ci/build.sh
