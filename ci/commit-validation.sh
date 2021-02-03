#!/bin/bash
set -xeuo pipefail
# Add cheap (non-building) checks here
dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/codestyle.sh
${dn}/ci-commitmessage-submodules.sh
