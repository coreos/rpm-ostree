#!/bin/bash
set -euo pipefail

# prepare ssh connection
vagrant ssh-config > ssh-config
echo "  ControlMaster auto" >> ssh-config
echo "  ControlPath $PWD/ssh.sock" >> ssh-config
echo "  ControlPersist yes" >> ssh-config
export SSH="ssh -F $PWD/ssh-config vmcheck"
export SCP="scp -F $PWD/ssh-config"

# stand up ssh connection and sanity check that it all works
if ! $SSH true &> /dev/null; then
  echo "ERROR: A running VM is required for 'make vmcheck'."
  exit 1
fi

LOG=${LOG:-"$PWD/vmcheck.log"}
echo -n '' > ${LOG}

testdir="$(dirname $(realpath $0))"
cd $testdir

failures=0
for tf in $(find . -name 'test-*.sh' | sort); do

    if [ -n "${TESTS+ }" ]; then
        tfbn=$(basename "$tf" .sh)
        tfbn=" ${tfbn#test-} "
        if [[ " $TESTS " != *$tfbn* ]]; then
            continue
        fi
    fi

    bn=$(basename ${tf})
    printf "Running $bn...\n"
    printf "\n==== ${tf} ====\n" >> ${LOG}

    # do some dirty piping to get some instant feedback and help debugging
    if ${tf} |& tee -a ${LOG} \
            | grep -e '^ok' --line-buffered \
            | xargs -d '\n' -n 1 echo "  "; then
        echo "PASS: $bn"
    else
        if test $? = 77; then
            echo "SKIP: $bn"
        else
            echo "FAIL: $bn"
            let "failures += 1"
        fi
    fi
done

# tear down ssh connection
$SSH -O exit &>/dev/null

if [ ${failures} -eq 0 ]; then
    echo "All tests passed."
else
    echo "Test failures: ${failures}"
    echo "See ${LOG} for more information."
    exit 1
fi
