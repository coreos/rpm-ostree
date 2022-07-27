#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `groupadd` calls.
# We want to learn about group creation and distinguish between
# static and dynamic GIDs, in order to auto-generate relevant
# `sysusers.d` fragments.
# See also https://github.com/coreos/rpm-ostree/issues/3762

if test -v RPMOSTREE_EXP_BRIDGE_SYSUSERS; then
    rpm-ostree scriptlet-intercept groupadd -- "$0" "$@"
fi

# Forward to the real `groupadd` for group creation.
exec /usr/sbin/groupadd.rpmostreesave "$@"
