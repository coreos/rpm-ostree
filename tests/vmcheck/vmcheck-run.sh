#!/bin/bash
# Run all vmcheck tests using parallel.  If
# VMCHECK_PARALLEL is set, it is a maximum number
# jobs to run in parallel.
#
# FIXME: dedup this logic with tests/compose
set -euo pipefail

export VMCHECK_TIMEOUT=${VMCHECK_TIMEOUT:-10m}

dn=$(cd $(dirname $0) && pwd)

# Preparatory work; we have a helper binary
make inject-pkglist
# And finally install the built binaries, which we'll then
# later rsync into the tree.
tests/vmcheck/install.sh

LOGDIR=${LOGDIR:-$(pwd)/vmcheck}
mkdir -p ${LOGDIR}
export LOGDIR

# From here, execute in a temporary directory
. ${dn}/../common/libtest.sh

all_tests="$(cd ${dn} && ls test-*.sh | sort)"
tests=""
# Support: env TESTS=misc make vmcheck
if [ -n "${TESTS+ }" ]; then
    for tf in ${all_tests}; do
        tfbn=$(basename "$tf" .sh)
        tfbn=" ${tfbn#test-} "
        if [[ " $TESTS " != *$tfbn* ]]; then
            echo "Skipping: ${tf}"
            continue
        fi
        tests="${tests} ${tf}"
    done
else
    # Used by papr to parallelize, e.g. run in parallel
    # TESTS_OFFSET=0 TESTS_MODULUS=3
    # TESTS_OFFSET=1 TESTS_MODULUS=3
    # TESTS_OFFSET=2 TESTS_MODULUS=3
    if [ -n "${TESTS_MODULUS+ }" ]; then
        o=0
        i=0
        for tf in ${all_tests}; do
            if [[ $(($o < ${TESTS_OFFSET})) == 1 ]]; then
                echo "o=$o Skipping: ${tf}"
                o=$((${o}+1))
                continue
            fi
            if [[ $i == 0 ]]; then
                tests="${tests} ${tf}"
            else
                echo "Skipping: ${tf}"
            fi
            i=$(((${i}+1) % ${TESTS_MODULUS}))
        done
    else
        tests="${all_tests}"
    fi
fi

if test -z "${tests}"; then
    fatal "error: No tests match ${TESTS}"
fi


echo "vmcheck tests starting: $(date)"
echo "Executing: ${tests}"
echo "Writing logs to ${LOGDIR}"

# Note we merge stdout/stderr here since I don't see value
# in separating them.
mkdir -p ${LOGDIR}/parallel
(for tf in ${tests}; do echo $tf; done) | \
    parallel -v -j ${VMCHECK_PARALLEL:-1} --progress --halt soon,fail=1 \
             --results ${LOGDIR}/parallel --quote /bin/sh -c "timeout --signal=KILL ${VMCHECK_TIMEOUT} ${dn}/run-one-test.sh {}"
echo "$(date): All vmcheck tests passed"
