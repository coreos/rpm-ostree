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
  '.deployments[0]["layered-commit-meta"]|not'
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
echo "ok polkit"

wrapdir="/usr/libexec/rpm-ostree/wrapped"
if [ -d "${wrapdir}" ]; then
    # Test wrapped functions for rpm
    rpm --version
    rpm -qa > /dev/null
    rpm --verify >out.txt
    assert_file_has_content out.txt "rpm --verify is not necessary for ostree-based systems"
    rm -f out.txt
    if rpm -e bash 2>out.txt; then
        fatal "rpm -e worked"
    fi
    assert_file_has_content out.txt 'Dropping privileges as `rpm` was executed with not "known safe" arguments'

    if dracut --blah 2>out.txt; then
        fatal "dracut worked"
    fi
    assert_file_has_content out.txt 'This system is rpm-ostree based'
    rm -f out.txt
else
    echo "Missing ${wrapdir}; cliwrap not enabled"
fi

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

# related: https://github.com/coreos/fedora-coreos-config/issues/194
rpm-ostree testutils moo
echo "ok moo"

# Reload as root https://github.com/projectatomic/rpm-ostree/issues/976
rpm-ostree reload
echo "ok reload"

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
