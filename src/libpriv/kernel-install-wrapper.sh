#!/usr/bin/bash
# Used in the container layering path to make kernel replacements Just Work
# without having to enable cliwrap first. If cliwrap is enabled, then this will
# technically override the cliwrap wrapper, but the script is exactly the same.
# This wrapper is technically also installed when doing client-side layering,
# but we already ignore kernel scriptlets there anyway.
# See also https://github.com/coreos/rpm-ostree/issues/4949

exec /usr/bin/rpm-ostree cliwrap kernel-install "$@"
