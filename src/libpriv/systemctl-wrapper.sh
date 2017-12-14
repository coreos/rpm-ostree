#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `systemctl` operations. We want to
# handle `preset`, and ignore everything else such as `start`/`stop` etc.
# See also https://github.com/projectatomic/rpm-ostree/issues/550

for arg in "$@"; do
    if [[ $arg == preset ]]; then
        exec /usr/bin/systemctl.rpmostreesave "$@"
    fi
done
echo "rpm-ostree-systemctl: Ignored non-preset command:" "$@"
