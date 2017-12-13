#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `systemctl` operations. We want to
# handle `preset`, and ignore everything else such as `start`/`stop` etc.
# See also https://github.com/projectatomic/rpm-ostree/issues/550

# Little helper function for reading args from the commandline.
# it automatically handles -a b and -a=b variants, and returns 1 if
# we need to shift $3.
read_arg() {
    # $1 = arg name
    # $2 = arg value
    # $3 = arg parameter
    local rematch='^[^=]*=(.*)$'
    if [[ $2 =~ $rematch ]]; then
        read "$1" <<< "${BASH_REMATCH[1]}"
    else
        read "$1" <<< "$3"
        # There is no way to shift our callers args, so
        # return 1 to indicate they should do it instead.
        return 1
    fi
}

do_preset() {
    exec /usr/bin/systemctl.rpmostreesave "$@"
}

# Ignore everything until we see `preset`, if we do then
# call the real systemctl.
while (($# > 0)); do
    case "${1%%=*}" in
        --*) ;;
        preset) do_preset "$@"; exit 0;;
        *) echo "rpm-ostree-systemctl: Ignoring: $1";;
    esac
    shift
done
