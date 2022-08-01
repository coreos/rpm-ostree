#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `useradd` calls.
# We want to learn about user creation and distinguish between
# static and dynamic IDs, in order to auto-generate relevant
# `sysusers.d` fragments.
# See also https://github.com/coreos/rpm-ostree/issues/3762

if test -v RPMOSTREE_EXP_BRIDGE_SYSUSERS; then
    rpm-ostree scriptlet-intercept useradd -- "$0" "$@"
fi

# Forward to the real `useradd` for group creation.
exec /usr/sbin/useradd.rpmostreesave "$@"
