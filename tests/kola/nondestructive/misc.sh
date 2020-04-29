#!/bin/bash
set -euo pipefail

. ${KOLA_EXT_DATA}/libtest-core.sh
cd $(mktemp -d)

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
