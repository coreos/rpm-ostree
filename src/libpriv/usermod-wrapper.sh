#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `usermod` calls.
# We want to learn about additional groups changes, in
# order to auto-generate relevant `sysusers.d` fragments.
# See also https://github.com/coreos/rpm-ostree/issues/3762

if test -v RPMOSTREE_EXP_BRIDGE_SYSUSERS; then
    rpm-ostree scriptlet-intercept usermod -- "$0" "$@"
fi

# Forward to the real `usermod` for group changes.
exec /usr/sbin/usermod.rpmostreesave "$@"
