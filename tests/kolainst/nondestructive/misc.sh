#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libtest.sh
cd $(mktemp -d)

libtest_prepare_offline
libtest_enable_repover 0

# Sanity-check the policy isn't marked as modified
if ostree admin config-diff | grep 'selinux/targeted/policy'; then
    assert_not_reached "selinux policy is marked as modified"
fi

# Verify that we process arguments correctly
rpm-ostree usroverlay --help >out.txt
assert_file_has_content out.txt "ostree admin unlock"
rm -f out.txt
rpm-ostree -h usroverlay >out.txt
assert_file_has_content out.txt "ostree admin unlock"

tmpfiles="/usr/lib/tmpfiles.d/rpm-ostree-autovar.conf"

# Verify /var/lib/selinux compatibility symlink (added in recent rpm-ostree)
integ="/usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf"
if grep -qF 'L /var/lib/selinux' "$integ"; then
  assert_file_has_content_literal "$integ" 'L /var/lib/selinux - - - - ../../etc/selinux'
  test -L /var/lib/selinux
  assert_streq "$(readlink /var/lib/selinux)" "../../etc/selinux"
  assert_not_file_has_content "$tmpfiles" '/var/lib/selinux'
  echo "ok /var/lib/selinux compatibility symlink"
else
  echo "ok /var/lib/selinux compatibility symlink (not present in this build, skipping)"
fi

# Verify https://github.com/coreos/rpm-ostree/issues/26
# Duplication in tmp.conf
assert_not_file_has_content $tmpfiles 'd /var/tmp'
# Duplication in var.conf
assert_not_file_has_content $tmpfiles 'd /var/cache '
assert_file_has_content_literal "$tmpfiles" 'd /var/lib/logrotate'

# https://github.com/coreos/rpm-ostree/issues/5040
# only check logs after switchroot
curosr=$(journalctl -u initrd-switch-root.service -o json -n 1 | jq -r '.["__CURSOR"]')
set +x # so our grepping doesn't get a hit on itself
if journalctl -u systemd-tmpfiles-setup.service --after-cursor ${curosr} --grep 'Duplicate line'; then
    fatal "Should not get logs (Duplicate line)"
fi
set -x # restore

# Verify remove can trigger the generation of rpm-ostree-autovar.conf
rpm-ostree override remove logrotate
deploy=$(rpm-ostree status --json | jq -r '.deployments[0]["id"]' | awk -F'-' '{print $3}')
osname=$(rpm-ostree status --json | jq -r '.deployments[0]["osname"]')
cat /ostree/deploy/${osname}/deploy/${deploy}/${tmpfiles} > autovar.txt
assert_not_file_has_content autovar.txt '/var/lib/logrotate'
rpm-ostree cleanup -pr

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
echo "ok empty pkg arrays, and commit meta correct in status json"

rpm-ostree status -b --json > status.json
assert_jq status.json '.deployments|length == 1'
echo "ok --booted --json"

# All tests which require a booted system, but are nondestructive
rpm-ostree testutils integration-read-only

systemctl show -p TimeoutStartUSec rpm-ostreed.service > out.txt
assert_file_has_content out.txt 'TimeoutStartUSec=5m'

# Ensure we return an error when passing a wrong option.
rpm-ostree --help | awk '/^$/ {in_commands=0} {if(in_commands==1){print $0}} /^Builtin Commands:/ {in_commands=1}' > commands.txt
while read cmd; do
    if rpm-ostree ${cmd} --n0t-3xisting-0ption &>/dev/null; then
        assert_not_reached "command ${cmd} --n0t-3xisting-0ption was successful"
    fi
done < commands.txt
echo "ok error on unknown command options"

if rpm-ostree status "--track=/etc/NetworkManager/system-connections`echo -n -e \"\xE2\x80\"`" 2>err.txt; then
    fatal "handled non UTF-8 args"
fi
assert_file_has_content_literal err.txt 'error: Argument is invalid UTF-8'
echo "ok error on non UTF-8"

if rpm-ostree ex rebuild 2>err.txt; then
    fatal "ex rebuild on host"
fi
assert_file_has_content_literal err.txt 'error: This command can only run in an OSTree container'

if rpm-ostree install --enablerepo=blah foo 2>err.txt; then
    fatal "enablerepo should not have worked"
fi
assert_file_has_content err.txt 'enablerepo currently only works in a container build'

rpm-ostree status --jsonpath '$.deployments[0].booted' > jsonpath.txt
assert_file_has_content_literal jsonpath.txt 'true'
echo "ok jsonpath"

rpmostree_busctl_call_os ListRepos > out.txt
assert_file_has_content_literal out.txt '"id" s "libtest"'
assert_file_has_content_literal out.txt '"description" s "libtest repo"'
assert_file_has_content_literal out.txt '"is-devel" b false'
assert_file_has_content_literal out.txt '"is-source" b false'
assert_file_has_content_literal out.txt '"is-enabled" b true'
echo "ok dbus ListRepos"

rpmostree_busctl_call_os WhatProvides as 1 provided-testing-daemon > out.txt
assert_file_has_content_literal out.txt '"epoch" t 0'
assert_file_has_content_literal out.txt '"reponame" s "libtest"'
assert_file_has_content_literal out.txt '"nevra" s "testdaemon'
rpmostree_busctl_call_os WhatProvides as 1 should-not-exist-p-equals-np > out.txt
assert_file_has_content_literal out.txt 'aa{sv} 0'
echo "ok dbus WhatProvides"

rpmostree_busctl_call_os GetPackages as 1 testdaemon > out.txt
assert_file_has_content_literal out.txt '"epoch" t 0'
assert_file_has_content_literal out.txt '"reponame" s "libtest"'
assert_file_has_content_literal out.txt '"nevra" s "testdaemon'
rpmostree_busctl_call_os GetPackages as 1 should-not-exist-p-equals-np > out.txt
assert_file_has_content_literal out.txt 'aa{sv} 0'
echo "ok dbus GetPackages"

rpmostree_busctl_call_os Search as 1 testdaemon > out.txt
assert_file_has_content_literal out.txt '"epoch" t 0'
assert_file_has_content_literal out.txt '"reponame" s "libtest"'
assert_file_has_content_literal out.txt '"nevra" s "testdaemon'
rpmostree_busctl_call_os Search as 1 should-not-exist-p-equals-np > out.txt
assert_file_has_content_literal out.txt 'aa{sv} 0'
echo "ok dbus Search"

rpm-ostree search testdaemon > out.txt
assert_file_has_content_literal out.txt '===== Name Matched ====='
assert_file_has_content_literal out.txt 'testdaemon : awesome-daemon-for-testing'
echo "ok Search name match"

rpm-ostree search awesome-daemon > out.txt
assert_file_has_content_literal out.txt '===== Summary Matched ====='
assert_file_has_content_literal out.txt 'testdaemon : awesome-daemon-for-testing'
echo "ok Search summary match"

rpm-ostree search testdaemon awesome-daemon > out.txt
assert_file_has_content_literal out.txt '===== Summary & Name Matched ====='
assert_file_has_content_literal out.txt 'testdaemon : awesome-daemon-for-testing'
echo "ok Search name and summary match"

rpm-ostree search "test*" > out.txt
assert_file_has_content_literal out.txt '===== Summary & Name Matched ====='
assert_file_has_content_literal out.txt '===== Name Matched ====='
assert_file_has_content_literal out.txt '===== Summary Matched ====='
assert_file_has_content_literal out.txt 'testdaemon : awesome-daemon-for-testing'
assert_file_has_content_literal out.txt 'testpkg-etc : testpkg-etc'
assert_file_has_content_literal out.txt 'testpkg-post-infinite-loop : testpkg-post-infinite-loop'
assert_file_has_content_literal out.txt 'testpkg-touch-run : testpkg-touch-run'
echo "ok Search glob pattern match"

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

# Verify we work without polkit, as root
systemctl mask --now polkit
systemctl restart rpm-ostreed
# This should work as root
rpm-ostree reload
# And non-root should still work for methods that don't need auth
runuser -u bin rpm-ostree status
# But these shouldn't work
if runuser -u core rpm-ostree reload 2>err.txt; then
    assert_not_reached "Was able to reload as non-root!"
fi
assert_file_has_content err.txt 'error: Authorization error:.*unit is masked'
rm -f err.txt
if runuser -u core -- rpm-ostree initramfs --enable 2>err.txt; then
    assert_not_reached "Was able to enable initramfs as non-root!"
fi
assert_file_has_content err.txt 'error: Authorization error:.*unit is masked'
systemctl unmask polkit
echo "ok worked without polkit"

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
if env FAILPOINTS='client::connect=return(synthetic-error)' rpm-ostree initramfs --enable 2>err.txt; then
    fatal "should have errored"
fi
assert_file_has_content_literal err.txt "error: synthetic-error"
journal_poll -u rpm-ostreed --after-cursor "${cursor}" --grep="client disconnected before calling Start"
rpm-ostree status > out.txt
assert_file_has_content_literal out.txt 'State: idle'
echo "ok auto-cancel not-started transaction"

# See rpmostree-scripts.c
grep ^DEFAULT /etc/crypto-policies/config
echo "ok crypto-policies DEFAULT backend"

ldd /usr/lib64/librpmostree-1.so.1 > rpmostree-lib-deps.txt
assert_not_file_has_content rpmostree-lib-deps.txt libdnf
echo "ok lib deps"

origin=$(rpm-ostree status --json | jq -r '.deployments[0].origin')
# Only run this test if we have a remote configured; this won't
# be the case for e.g. cosa build-fast.
case "$origin" in
    *:*) 
    mv /etc/ostree/remotes.d{,.orig}
    systemctl restart rpm-ostreed
    rpm-ostree status > status.txt
    assert_file_has_content status.txt 'Remote.*not found'
    mv /etc/ostree/remotes.d{.orig,}
    rpm-ostree reload
    echo "ok remote not found"
    ;;
esac

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

# This just verifies --register-driver
rpm-ostree deploy --register-driver "foo"
rpm-ostree status > status.txt
assert_file_has_content status.txt "AutomaticUpdatesDriver: foo"

# Verify with wrong proxy, rpm-ostree rebase failed as expected
# https://issues.redhat.com/browse/OCPBUGS-27200
mkdir -p /etc/systemd/system/rpm-ostreed.service.d
cat > /etc/systemd/system/rpm-ostreed.service.d/http-proxy.conf << EOF
[Service]
Environment="http_proxy=http://test123.com:3128"
Environment="https_proxy=https://test123.com:3128"
EOF
systemctl daemon-reload
systemctl restart rpm-ostreed.service
if rpm-ostree rebase ostree-unverified-registry:quay.io/fedora/fedora-coreos:testing-devel &> err.txt; then
    echo "should not success with wrong proxy"
    exit 1
fi
assert_file_has_content_literal err.txt "proxyconnect tcp: dial tcp: lookup test123.com"
echo "ok proxy"

# Test GHSA-2m78-7qj3-jmvc: treefile modifier must require override action
# The treefile modifier in UpdateDeployment must map to the
# org.projectatomic.rpmostree1.override polkit action. Without the fix, a
# caller with only install-uninstall-packages permission could include treefile
# alongside another modifier and bypass the override requirement.
rm -f /etc/systemd/system/rpm-ostreed.service.d/http-proxy.conf
systemctl daemon-reload
systemctl restart rpm-ostreed.service

# Give core the weaker package-management permission while explicitly denying
# override.  This reproduces the authorization split required for the bypass:
# package changes must be accepted, but adding treefile must be rejected.
mkdir -p /etc/polkit-1/rules.d
cat >/etc/polkit-1/rules.d/49-rpm-ostree-treefile-test.rules <<'EOF'
polkit.addRule(function(action, subject) {
    if (subject.user == "core") {
        if (action.id == "org.projectatomic.rpmostree1.install-uninstall-packages")
            return polkit.Result.YES;
        if (action.id == "org.projectatomic.rpmostree1.override")
            return polkit.Result.NO;
    }
});
EOF
systemctl restart polkit

# Resolve the booted OS object path
stateroot=$(rpm-ostree status --booted --json | jq -r '.deployments[0].osname')
OSPATH=/org/projectatomic/rpmostree1/${stateroot//-/_}
# First prove that core is authorized for a package-only request.  The client
# disconnects without calling Start, so this does not alter the deployment.
runuser -u core -- gdbus call --system \
    --dest org.projectatomic.rpmostree1 \
    --object-path "$OSPATH" \
    --method org.projectatomic.rpmostree1.OS.UpdateDeployment \
    "{'install-packages': <['bash']>}" \
    "{'no-pull-base': <true>}" >/dev/null
echo "ok package-only UpdateDeployment allowed"

# The same permitted request plus treefile must require override and be denied.
if runuser -u core -- gdbus call --system \
    --dest org.projectatomic.rpmostree1 \
    --object-path "$OSPATH" \
    --method org.projectatomic.rpmostree1.OS.UpdateDeployment \
    "{'install-packages': <['bash']>, 'treefile': <'{\"packages\":[\"bash\"]}'>}" \
    "{'no-pull-base': <true>}" 2>err.txt; then
    assert_not_reached "install-packages+treefile UpdateDeployment should require override"
fi
assert_file_has_content err.txt 'Authorization error\|AccessDenied'
echo "ok install-packages+treefile UpdateDeployment denied without override permission"

rm -f /etc/polkit-1/rules.d/49-rpm-ostree-treefile-test.rules
systemctl restart polkit

# Test GHSA-rq45-x7mc-f7h8: transaction socket connection limit and UID restriction
# The transaction progress socket must:
# 1. Reject connections once the peer limit is reached
# 2. Reject connections from UIDs other than root and the transaction initiator
rm -f /etc/systemd/system/rpm-ostreed.service.d/http-proxy.conf
mkdir -p /etc/systemd/system/rpm-ostreed.service.d
cat > /etc/systemd/system/rpm-ostreed.service.d/test-failpoint.conf <<'EOF'
[Service]
Environment="FAILPOINTS=transaction::execute=sleep(5000)"
EOF
systemctl daemon-reload
systemctl restart rpm-ostreed.service

# Hold a transaction in its execute method so the progress socket remains
# available while connections are authenticated.
rpm-ostree refresh-md --force &
REFRESH_PID=$!
for _ in $(seq 1 50); do
    if test -S /run/rpm-ostree-transaction.sock; then
        break
    fi
    sleep 0.2
done
test -S /run/rpm-ostree-transaction.sock

# A real D-Bus client is required to complete authentication and reach the
# new-connection callback; a raw Unix socket connection is insufficient.
cursor=$(journalctl -o json -n 1 | jq -r '.["__CURSOR"]')
timeout 2 runuser -u bin -- gdbus monitor \
    --address=unix:path=/run/rpm-ostree-transaction.sock \
    --object-path / >/dev/null 2>&1 || true
for _ in $(seq 1 20); do
    if journalctl -u rpm-ostreed --after-cursor "$cursor" \
        | grep -q 'is not the transaction initiator'; then
        break
    fi
    sleep 0.1
done
journalctl -u rpm-ostreed --after-cursor "$cursor" \
    | grep -q 'is not the transaction initiator'
echo "ok transaction socket UID restriction"

# The initiating rpm-ostree client occupies one retained peer slot.  Hold 63
# additional authenticated connections open as root to reach the limit of 64.
cursor=$(journalctl -o json -n 1 | jq -r '.["__CURSOR"]')
PEER_PIDS=()
for _ in $(seq 1 63); do
    { printf '\0AUTH EXTERNAL 30\r\nBEGIN\r\n'; sleep 30; } \
        | socat - UNIX-CONNECT:/run/rpm-ostree-transaction.sock >/dev/null &
    PEER_PIDS+=("$!")
done
for _ in $(seq 1 50); do
    if journalctl -u rpm-ostreed --after-cursor "$cursor" \
        | grep -q '(64/64 peers)'; then
        break
    fi
    sleep 0.1
done
journalctl -u rpm-ostreed --after-cursor "$cursor" \
    | grep -q '(64/64 peers)'

# One more authenticated, authorized connection must be rejected by the cap.
timeout 2 gdbus monitor \
    --address=unix:path=/run/rpm-ostree-transaction.sock \
    --object-path / >/dev/null 2>&1 || true
for _ in $(seq 1 20); do
    if journalctl -u rpm-ostreed --after-cursor "$cursor" \
        | grep -q 'peer limit reached (64/64)'; then
        break
    fi
    sleep 0.1
done
journalctl -u rpm-ostreed --after-cursor "$cursor" \
    | grep -q 'peer limit reached (64/64)'
echo "ok transaction socket authenticated peer limit"

kill "${PEER_PIDS[@]}" 2>/dev/null || true
wait "${PEER_PIDS[@]}" 2>/dev/null || true
wait $REFRESH_PID 2>/dev/null || true
rm -f /etc/systemd/system/rpm-ostreed.service.d/test-failpoint.conf
systemctl daemon-reload
systemctl restart rpm-ostreed.service
