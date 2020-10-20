#!/bin/bash
# Run a shell (or program) like how rpm-ostree would run RPM scriptlets. Useful
# for quickly testing changes to the script environment.
rootfs=$1
shift
cd ${rootfs}
# ⚠⚠⚠ If you change this, also update src/libpriv/rpmostree-scripts.c ⚠⚠⚠
BWRAP_ARGV="--dev /dev --proc /proc --dir /tmp --dir /run --chdir / \
     --unshare-pid --unshare-uts \
     --unshare-ipc --unshare-cgroup-try \
"
if ! test "${container:-}" = "systemd-nspawn"; then
    BWRAP_ARGV="$BWRAP_ARGV --unshare-net"
fi

for src in /sys/{block,bus,class,dev}; do
    BWRAP_ARGV="$BWRAP_ARGV --ro-bind $src $src"
done
for src in lib{,32,64} bin sbin; do
    if test -L $src; then
        BWRAP_ARGV="$BWRAP_ARGV --symlink usr/$src $src"
    fi
done
BWRAP_ARGV="$BWRAP_ARGV --ro-bind usr /usr --ro-bind ./var /var --bind ./usr/etc /etc \
            --tmpfs /var/tmp --tmpfs /var/lib/rpm-state"
echo exec bwrap $BWRAP_ARGV "$@"
exec env PS1='bwrap$ ' bwrap $BWRAP_ARGV "$@"
