#!/bin/bash
set -euo pipefail

if [ -n "${V:-}" ]; then
  set -x
fi

outputdir=$1; shift
testname=$1; shift

outputdir="${outputdir}/${testname}"
rm -rf "${outputdir:?}"/*
mkdir -p "${outputdir}"

# keep original stdout around; this propagates to the terminal
exec 3>&1

# but redirect everything else to a log file
exec 1>"${outputdir}/output.log"
exec 2>&1

# seed output log with current date
date

if [ -n "${V:-}" ]; then
  setpriv --pdeathsig SIGKILL -- tail -f "${outputdir}/output.log" >&3 &
fi

echo "EXEC: ${testname}" >&3

# this will cause libtest.sh to allocate a tmpdir and cd to it
export VMTESTS=1

# shellcheck source=../common/libtest.sh disable=2154
. "${commondir}/libtest.sh"

# shellcheck source=../common/libvm.sh
. "${commondir}/libvm.sh"

vm_kola_spawn "${outputdir}/kola"
if "${topsrcdir}/tests/vmcheck/test-${testname}.sh"; then
  echo "PASS: ${testname}" >&3
else
  echo "FAIL: ${testname}" >&3
  if [ -z "${V:-}" ]; then
    tail -n20 "${outputdir}/output.log" | sed "s/^/   ${testname}: /g" >&3
  fi

  if [ -n "${VMCHECK_DEBUG:-}" ]; then
    echo "--- VMCHECK_DEBUG ---" >&3
    echo "To try SSH:" "SSH_AUTH_SOCK=$(realpath "${SSH_AUTH_SOCK}") ${SSH:-}" >&3
    echo "Sleeping..." >&3
    sleep infinity
  fi
  exit 1
fi
