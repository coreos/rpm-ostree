#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libtest.sh
cd $(mktemp -d)

# make sure that package-related entries are always present,
# even when they're empty.
# Validate there's no live state by default.
rpm-ostree status --json > status.json
assert_jq status.json \
  '.deployments[0]["packages"]' \
  '.deployments[0]["requested-packages"]' \
  '.deployments[0]["requested-local-packages"]' \
  '.deployments[0]["base-removals"]' \
  '.deployments[0]["requested-base-removals"]' \
  '.deployments[0]["live-inprogress"]|not' \
  '.deployments[0]["live-replaced"]|not' \
  '.deployments[0]["layered-commit-meta"]|not' \
  '.deployments[0]["staged"]|not'
rm status.json
rpm-ostree testutils validate-parse-status
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

if runuser -u core rpm-ostree pkg-add foo &>err.txt; then
    fatal "Was able to install a package as non-root!"
fi
assert_file_has_content err.txt 'PkgChange not allowed for user'
if runuser -u core rpm-ostree reload &>err.txt; then
    assert_not_reached "Was able to reload as non-root!"
fi

# StateRoot is only in --verbose, also verify we're not showing
# unlocked.
rpm-ostree status > status.txt
assert_not_file_has_content status.txt StateRoot:
assert_not_file_has_content status.txt Unlocked:
rpm-ostree status -v > status.txt
assert_file_has_content status.txt StateRoot:
echo "ok status text"

# Also check that we can do status as non-root non-active
runuser -u bin rpm-ostree status
echo "ok status doesn't require active PAM session"

rpm-ostree status -b > status.txt
assert_file_has_content status.txt BootedDeployment:
echo "ok status -b"

if rpm-ostree nosuchcommand --nosuchoption 2>err.txt; then
    assert_not_reached "Expected an error for nosuchcommand"
fi
assert_file_has_content err.txt 'Unknown.*command'
echo "ok error on unknown command"

# related: https://github.com/coreos/fedora-coreos-config/issues/194
rpm-ostree testutils moo
echo "ok moo"

# Reload as root https://github.com/projectatomic/rpm-ostree/issues/976
rpm-ostree reload
echo "ok reload"

cursor=$(journalctl -o json -n 1 | jq -r '.["__CURSOR"]')
if env FAILPOINTS=client::connect=panic rpm-ostree initramfs --enable 2>err.txt; then
    fatal "should have errored"
fi
assert_file_has_content_literal err.txt "failpoint client::connect panic"
journalctl -u rpm-ostreed --after-cursor "${cursor}" > out.txt
assert_file_has_content_literal out.txt 'client disconnected before calling Start'
rpm-ostree status > out.txt
assert_file_has_content_literal out.txt 'State: idle'
echo "ok auto-cancel not-started transaction"

# See rpmostree-scripts.c
grep ^DEFAULT /etc/crypto-policies/config
echo "ok crypto-policies DEFAULT backend"

ldd /usr/lib64/librpmostree-1.so.1 > rpmostree-lib-deps.txt
assert_not_file_has_content rpmostree-lib-deps.txt libdnf
echo "ok lib deps"

mv /etc/ostree/remotes.d{,.orig}
systemctl restart rpm-ostreed
rpm-ostree status > status.txt
assert_file_has_content status.txt 'Remote.*not found'
mv /etc/ostree/remotes.d{.orig,}
rpm-ostree reload
echo "ok remote not found"

systemctl stop rpm-ostreed
mv /var/lib/rpm{,.orig}
cp -a $(realpath /usr/share/rpm) /var/lib/rpm
if systemctl start rpm-ostreed; then
    fatal "Started rpm-ostreed with /var/lib/rpm"
fi
rm /var/lib/rpm -rf
mv /var/lib/rpm{.orig,}
systemctl reset-failed rpm-ostreed
echo "ok validated rpmdb"

systemctl stop rpm-ostreed
unshare -m /bin/bash -c 'mount -o remount,rw /boot && mkdir /boot/orig-loader && mv /boot/loader* /boot/orig-loader'
if rpm-ostree status &>err.txt; then
    fatal "started rpm-ostreed with no /boot/loader"
fi
assert_file_has_content_literal err.txt "Unexpected state: /run/ostree-booted found, but no /boot/loader directory"
rm -f err.txt
unshare -m /bin/bash -c 'mount -o remount,rw /boot && mv /boot/orig-loader/* /boot'
systemctl restart rpm-ostreed
echo "ok daemon statup failure gives useful error"

rpm-ostree cleanup -p
originpath=$(ostree admin --print-current-dir).origin
unshare -m /bin/bash -c "mount -o remount,rw /sysroot && cp -a ${originpath}{,.orig} && 
   echo 'unconfigured-state=Access to TestOS requires ONE BILLION DOLLARS' >> ${originpath}"
rpm-ostree reload
rpm-ostree status
if rpm-ostree upgrade 2>err.txt; then
    echo "Upgraded from unconfigured-state"
    exit 1
fi
grep -qFe 'ONE BILLION DOLLARS' err.txt
unshare -m /bin/bash -c "mount -o remount,rw /sysroot && cp -a ${originpath}{.orig,}"
rpm-ostree reload
echo "ok unconfigured-state"

### Stuff following here may mutate the host persistently ###

rpm-ostree usroverlay
echo some content > /usr/share/testcontent
echo "ok usroverlay"
