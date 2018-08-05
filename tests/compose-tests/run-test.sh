#!/bin/bash
set -euo pipefail

tf=$1
export TEST_ARTIFACTS=${LOGDIR}/${tf}
mkdir -p ${TEST_ARTIFACTS}
# Redirect our stdout/stderr, since we don't want what GNU parallel does
exec 1>${TEST_ARTIFACTS}/output.txt
exec 2>&1
# Rename the dir itself if non-zero rc to make it easy to know what failed
rc=0
$(dirname $0)/${tf} || rc=$?
if [ $rc == 0 ]; then
  mv ${TEST_ARTIFACTS}{,.pass}
else
  mv ${TEST_ARTIFACTS}{,.fail.$rc}
fi
