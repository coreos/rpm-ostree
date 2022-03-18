#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd "$dn/.." && pwd)
commondir=$(cd "$dn/common" && pwd)
export topsrcdir commondir

# shellcheck source=common/libtest-core.sh
. "${commondir}/libtest-core.sh"

read -r -a tests <<< "$(filter_tests "${topsrcdir}/tests/vmcheck")"
if [ ${#tests[*]} -eq 0 ]; then
  echo "No tests selected; mistyped filter?"
  exit 0
fi

JOBS=${JOBS:-$(ncpus)}

echo "Running ${#tests[*]} tests ${JOBS} at a time"

outputdir="${topsrcdir}/vmcheck-logs"
echo "Test results outputting to ${outputdir}/"

echo -n "${tests[*]}" | parallel -d' ' -j "${JOBS}" --line-buffer \
  "${topsrcdir}/tests/vmcheck/runtest.sh" "${outputdir}"
