#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `systemctl` operations. We want to
# handle `preset`, and ignore everything else such as `start`/`stop` etc.
# However if --root is passed, we do want to support that.
# See also https://github.com/projectatomic/rpm-ostree/issues/550

for arg in "$@"; do
    case $arg in
        preset | --root | --root=* | enable | disable) exec /usr/bin/systemctl.rpmostreesave "$@" ;;
    esac
done
echo "rpm-ostree-systemctl: Ignored non-preset command:" "$@"
