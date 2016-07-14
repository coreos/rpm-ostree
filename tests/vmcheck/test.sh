#!/bin/bash
set -euo pipefail

# prepare ssh connection

# If there's already an ssh-config, just use that one. The user might have
# created it for a self-provisioned machine. Otherwise, let's just assume we're
# using vagrant and generate an ssh-config.
if [ ! -f ssh-config ]; then
  vagrant ssh-config > ssh-config
  echo "  ControlMaster auto" >> ssh-config
  echo "  ControlPath $PWD/ssh.sock" >> ssh-config
  echo "  ControlPersist yes" >> ssh-config
fi

export SSH="ssh -F $PWD/ssh-config vmcheck"
export SCP="scp -F $PWD/ssh-config"

. ${commondir}/libvm.sh

# stand up ssh connection and sanity check that it all works
if ! vm_ssh_wait 20; then
  echo "ERROR: A running VM is required for 'make vmcheck'."
  exit 1
fi

# just error out if we're unlocked -- we use the current deployment as the
# fallback between each test, so we need to be sure it's in a state that works.
# also, the user might have forgotten that these tests are somewhat destructive
# and thus would wipe out unlocked changes, hotfix or not.
unlocked_cur=$(vm_get_booted_deployment_info unlocked)
if [[ $unlocked_cur != none ]]; then
  echo "ERROR: VM is unlocked."
  exit 1
fi

# remember the csum we're currently on and tag it so that ostree doesn't wipe it
csum_orig=$(vm_get_booted_csum)
vm_cmd ostree rev-parse $csum_orig &> /dev/null # validate
vm_cmd ostree refs vmcheck_orig --delete
vm_cmd ostree refs $csum_orig --create vmcheck_orig

LOG=${LOG:-"$PWD/vmcheck.log"}
echo -n '' > ${LOG}

testdir="$(dirname $(realpath $0))"
cd $testdir

colour_print() {
  colour=$1; shift
  [ ! -t 1 ] || echo -en "\e[${colour}m"
  echo -n "$@"
  [ ! -t 1 ] || echo -en "\e[0m"
  echo
}

pass_print() {
  colour_print 32 "$@" # green
}

fail_print() {
  colour_print 31 "$@" # red
}

skip_print() {
  colour_print 34 "$@" # blue
}

total=0
pass=0
fail=0
skip=0
for tf in $(find . -name 'test-*.sh' | sort); do

    if [ -n "${TESTS+ }" ]; then
        tfbn=$(basename "$tf" .sh)
        tfbn=" ${tfbn#test-} "
        if [[ " $TESTS " != *$tfbn* ]]; then
            continue
        fi
    fi

    let "total += 1"

    bn=$(basename ${tf})
    printf "Running $bn...\n"
    printf "\n==== ${tf} ====\n" >> ${LOG}

    # do some dirty piping to get some instant feedback and help debugging
    if ${tf} |& tee -a ${LOG} \
            | grep -e '^ok ' --line-buffered \
            | xargs -d '\n' -n 1 echo "  "; then
        pass_print "PASS: $bn"
        let "pass += 1"
    else
        if test $? = 77; then
            skip_print "SKIP: $bn"
            let "skip += 1"
        else
            fail_print "FAIL: $bn"
            let "fail += 1"
        fi
    fi

    # go back to the original vmcheck deployment if needed
    csum_cur=$(vm_get_booted_csum)
    unlocked_cur=$(vm_get_booted_deployment_info unlocked)
    if [[ $csum_orig != $csum_cur ]] || \
       [[ $unlocked_cur != none ]]; then
      # redeploy under the name 'vmcheck' so that tests can never modify the
      # vmcheck_orig ref itself (e.g. package layering)
      echo "Restoring vmcheck commit" >> ${LOG}
      vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_orig &>> ${LOG}
      vm_cmd ostree admin deploy vmcheck &>> ${LOG}
      vm_reboot
    fi
done

# tear down ssh connection if needed
if $SSH -O check &>/dev/null; then
    $SSH -O exit &>/dev/null
fi

[ ${fail} -eq 0 ] && printer=pass || printer=fail
${printer}_print "TOTAL: $total PASS: $pass SKIP: $skip FAIL: $fail"
echo "See ${LOG} for more information."
[ ${fail} -eq 0 ]
