#!/bin/bash
set -euo pipefail

. ${KOLA_EXT_DATA}/libtest-core.sh
cd $(mktemp -d)

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
