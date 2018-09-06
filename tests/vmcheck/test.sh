#!/bin/bash
set -euo pipefail

. ${commondir}/libvm.sh

LOG=${LOG:-vmcheck.log}
LOG=$(realpath $LOG)

# NB: allow JOURNAL_LOG to be empty, which means we never
# fetch it
JOURNAL_LOG=${JOURNAL_LOG-vmcheck-journal.txt}
if [ -n "$JOURNAL_LOG" ]; then
  JOURNAL_LOG=$(realpath $JOURNAL_LOG)
fi

# create ssh-config if needed and export cmds
vm_setup

# stand up ssh connection and sanity check that it all works
if ! vm_ssh_wait 30; then
  echo "ERROR: A running VM is required for 'make vmcheck'."
  exit 1
fi

echo "VM is running."

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

# reboot onto the vmcheck ref if we're not already on it
origin=$(vm_get_booted_deployment_info origin)
if [[ $origin != vmcheck ]]; then
  vm_cmd ostree refs vmcheck --delete
  vm_cmd ostree refs vmcheck_orig --create vmcheck
  vm_cmd ostree admin deploy vmcheck &>> ${LOG}
  vm_reboot &>> ${LOG}
fi

# delete whatever tmp refs the previous testsuite runs may have created
vm_cmd ostree refs vmcheck_tmp vmcheck_remote --delete

# we bring our own test repo and test packages, so let's neuter any repo that
# comes with the distro to help speed up rpm-ostree metadata fetching since we
# don't cache it (e.g. on Fedora, it takes *forever* to fetch metadata, which we
# have to do dozens of times throughout the suite)
vm_cmd mkdir -p /etc/yum.repos.d/
if ! vm_cmd test -f /etc/yum.repos.d/.vmcheck; then
    echo "Neutering /etc/yum.repos.d"
    # Move the current one to .bak
    vm_cmd mv /etc/yum.repos.d{,.bak}
    # And create a new one with a .vmcheck as a stamp file so we recognize it
    vm_cmd rm /etc/yum.repos.d.tmp -rf
    vm_cmd mkdir /etc/yum.repos.d.tmp
    vm_cmd touch /etc/yum.repos.d.tmp/.vmcheck
    vm_cmd cp -r /etc/yum.repos.d{.tmp,}
else
    echo "Keeping existing vmcheck /etc/yum.repos.d"
fi

# tests expect to run with the default config
# remember the original config, we restore it after the tests
if vm_cmd test -f /etc/rpm-ostreed.conf; then
  vm_cmd mv -f /etc/rpm-ostreed.conf{,.bak}
fi
if vm_cmd test -f /usr/etc/rpm-ostreed.conf; then
    vm_cmd cp -f /usr/etc/rpm-ostreed.conf /etc
    # Unless we're doing overrides
    if [[ "${VMCHECK_FLAGS:-}" =~ "not-stage-deployments" ]]; then
        vm_cmd 'echo "[Experimental]" >> /etc/rpm-ostreed.conf'
        vm_cmd 'echo StageDeployments=false >> /etc/rpm-ostreed.conf'
    fi
fi

vm_cmd ostree remote delete --if-exists vmcheckmote

origdir=$(pwd)
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
notrun=0
for tf in $(find . -name 'test-*.sh' | sort); do

    if [ -n "${TESTS+ }" ]; then
        tfbn=$(basename "$tf" .sh)
        tfbn=" ${tfbn#test-} "
        if [[ " $TESTS " != *$tfbn* ]]; then
            let "notrun += 1"
            continue
        fi
    fi

    let "total += 1"

    bn=$(basename ${tf})
    printf "Running $bn...\n"
    printf "\n==== ${bn} ====\n" >> ${LOG}
    vm_cmd logger "vmcheck: running $bn..."

    # do some dirty piping to get some instant feedback and help debugging
    if ${tf} |& tee -a ${LOG} \
            | grep -e '^ok ' --line-buffered \
            | xargs -d '\n' -n 1 echo "  "; then
        pass_print "PASS: $bn"
        echo "PASS" >> ${LOG}
        let "pass += 1"
    else
        if test $? = 77; then
            skip_print "SKIP: $bn"
            echo "SKIP" >> ${LOG}
            let "skip += 1"
        else
            fail_print "FAIL: $bn"
            echo "FAIL" >> ${LOG}
            let "fail += 1"
        fi
    fi

    vm_cmd logger "vmcheck: finished $bn..."
    if test -n "${VMCHECK_DEBUG:-}"; then
        echo "VMCHECK_DEBUG is set, skipping restoration of original deployment"
        break
    fi

    # nuke all vmcheck and vmcheck_tmp refs and recreate vmcheck from orig
    echo "Restoring original vmcheck commit" >> ${LOG}
    vm_cmd ostree refs vmcheck vmcheck_tmp vmcheck_remote --delete
    vm_cmd ostree refs vmcheck_orig --create vmcheck &>> ${LOG}

    # restore the default config
    vm_cmd cp -f /usr/etc/rpm-ostreed.conf /etc/

    # go back to the original vmcheck deployment if needed
    origin_cur=$(vm_get_booted_deployment_info origin)
    csum_cur=$(vm_get_booted_csum)
    unlocked_cur=$(vm_get_booted_deployment_info unlocked)
    live_csum=$(vm_get_booted_deployment_info live-replaced)
    if [[ $origin_cur != vmcheck ]] || \
       [[ $csum_orig != $csum_cur ]] || \
       [[ $unlocked_cur != none ]] || \
       [ -n "${live_csum}" ]; then
      # redeploy under the name 'vmcheck' so that tests can
      # never modify the vmcheck_orig ref itself
      vm_cmd ostree admin deploy vmcheck &>> ${LOG}
      vm_reboot &>> ${LOG}
    else
      # make sure we're using the default config vals again
      vm_cmd systemctl restart rpm-ostreed
    fi

    # make sure to clean up any pending & rollback deployments
    vm_rpmostree cleanup -b -p -r -m || :

    # and put back our tmp repo
    vm_cmd rm /etc/yum.repos.d -rf
    vm_cmd cp -r /etc/yum.repos.d{.tmp,}

    # and clean up any leftovers from our tmp
    osname=$(vm_get_booted_deployment_info osname)
    vm_cmd rm -rf /ostree/deploy/$osname/var/tmp/vmcheck
    vm_cmd ostree remote delete --if-exists vmcheckmote
done


if test -z "${VMCHECK_DEBUG:-}"; then

    # put back the original yum repos
    if vm_cmd test -f /etc/yum.repos.d/.vmcheck; then
        echo "Restoring original /etc/yum.repos.d"
        vm_cmd rm -rf /etc/yum.repos.d
        vm_cmd mv /etc/yum.repos.d{.bak,}
    fi

    # put back the original config if any
    vm_cmd rm -f /etc/rpm-ostreed.conf
    if vm_cmd test -f /etc/rpm-ostreed.conf.bak; then
        vm_cmd mv /etc/rpm-ostreed.conf{.bak,}
    fi
fi

# Gather post-failure system logs if necessary
ALL_LOGS=$LOG
if [ ${fail} -ne 0 ] && [ -n "$JOURNAL_LOG" ]; then
    ./fetch-journal.sh
    ALL_LOGS="$ALL_LOGS $JOURNAL_LOG"
fi

# tear down ssh connection if needed
if $SSH -O check &>/dev/null; then
    $SSH -O exit &>/dev/null
fi

[ ${fail} -eq 0 ] && printer=pass || printer=fail
${printer}_print "TOTAL: $total PASS: $pass SKIP: $skip FAIL: $fail"
if test ${notrun} -gt 0; then
    echo "NOTICE: Skipped ${notrun} tests not matching \"${TESTS}\""
fi
echo "See ${ALL_LOGS} for more information."
[ ${fail} -eq 0 ]
