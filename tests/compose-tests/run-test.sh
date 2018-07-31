#!/bin/bash
set -euo pipefail

tf=$1
export TEST_ARTIFACTS=${LOGDIR}/${tf}
mkdir -p ${TEST_ARTIFACTS}
# Redirect our stdout/stderr, since we don't want what GNU parallel does
exec 1>${TEST_ARTIFACTS}/output.txt
exec 2>&1
exec $(dirname $0)/${tf}
