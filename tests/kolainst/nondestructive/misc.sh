#!/bin/bash
set -euo pipefail

. ${KOLA_EXT_DATA}/libtest.sh
cd $(mktemp -d)

# make sure that package-related entries are always present,
# even when they're empty
rpm-ostree status --json > status.json
assert_jq status.json \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]' \
  '.deployments[0]["requested-local-packages"]' \
  '.deployments[0]["base-removals"]' \
  '.deployments[0]["requested-base-removals"]' \
  '.deployments[0]["layered-commit-meta"]|not'
rm status.json
echo "ok empty pkg arrays, and commit meta correct in status json"

# Ensure we return an error when passing a wrong option.
rpm-ostree --help | awk '/^$/ {in_commands=0} {if(in_commands==1){print $0}} /^Builtin Commands:/ {in_commands=1}' > commands.txt
while read cmd; do
    if rpm-ostree ${cmd} --n0t-3xisting-0ption &>/dev/null; then
        assert_not_reached "command ${cmd} --n0t-3xisting-0ption was successful"
    fi
done < commands.txt
echo "ok error on unknown command options"

rpm-ostree status --jsonpath '$.deployments[0].booted' > jsonpath.txt
assert_file_has_content_literal jsonpath.txt 'true'
echo "ok jsonpath"

# Verify operations as non-root
runuser -u core rpm-ostree status
echo "ok status doesn't require root"

# StateRoot is only in --verbose
rpm-ostree status > status.txt
assert_not_file_has_content status.txt StateRoot:
rpm-ostree status -v > status.txt
assert_file_has_content status.txt StateRoot:
echo "ok status text"

# Also check that we can do status as non-root non-active
runuser -u bin rpm-ostree status
echo "ok status doesn't require active PAM session"

rpm-ostree status -b > status.txt
assert_streq $(grep -F -e 'ostree://' status.txt | wc -l) "1"
assert_file_has_content status.txt BootedDeployment:
echo "ok status -b"

if rpm-ostree nosuchcommand --nosuchoption 2>err.txt; then
    assert_not_reached "Expected an error for nosuchcommand"
fi
assert_file_has_content err.txt 'Unknown.*command'
echo "ok error on unknown command"

stateroot=$(dirname $(ls /ostree/deploy/*/var))
ospath=/org/projectatomic/rpmostree1/${stateroot//-/_}
# related: https://github.com/coreos/fedora-coreos-config/issues/194
(export LANG=C.utf8
 # And for some reason this one is set in kola runs but not interactive shells
 unset LC_ALL
 gdbus call \
  --system --dest org.projectatomic.rpmostree1 \
  --object-path /org/projectatomic/rpmostree1/fedora_coreos \
  --method org.projectatomic.rpmostree1.OSExperimental.Moo true > moo.txt
 assert_file_has_content moo.txt '🐄')
echo "ok moo"

tmprootfs=$(mktemp -d -p /var/tmp)
rpm-ostree coreos-rootfs seal "${tmprootfs}"
lsattr -d "${tmprootfs}" > coreos-rootfs.txt
rpm-ostree coreos-rootfs seal "${tmprootfs}"
assert_file_has_content coreos-rootfs.txt '-*i-* '"${tmprootfs}"
chattr -i "${tmprootfs}"
rm -rf "${tmprootfs}" coreos-rootfs.txt
echo "ok coreos-rootfs seal"

# Reload as root https://github.com/projectatomic/rpm-ostree/issues/976
rpm-ostree reload
echo "ok reload"

# See rpmostree-scripts.c
grep ^DEFAULT /etc/crypto-policies/config
echo "ok crypto-policies DEFAULT backend"
