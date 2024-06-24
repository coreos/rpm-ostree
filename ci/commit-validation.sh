#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -xeuo pipefail
# Add cheap (non-building) checks here
dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/codestyle.sh
${dn}/ci-commitmessage-submodules.sh
