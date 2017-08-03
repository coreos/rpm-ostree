#!/bin/bash
set -euo pipefail

if test -z "${INSIDE_VM:-}"; then

    # do this in the host
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required for 'make vmcheck'."
      exit 1
    fi

    set -x

    cd ${topsrcdir}
    export VMCHECK_INSTTREE=${VMCHECK_INSTTREE:-$(pwd)/insttree}

    # Always pull ostree from the build container; we assume development and
    # testing is against git master. See also overlay.sh and build.sh.
    ostree --version
    for pkg in ostree{,-libs,-grub2}; do
       rpm -q $pkg
       # We do not have perms to read /etc/grub2 as non-root. In the prebuilt
       # container case, manpages are missing. Ignore that.
       rpm -ql $pkg | grep -vE '^/(etc|usr/share/(doc|man))/' >  list.txt
       # Also chown everything to writable, due to
       # https://bugzilla.redhat.com/show_bug.cgi?id=517575
       chmod -R u+w ${VMCHECK_INSTTREE}/
       # Note we can't use --ignore-missing-args here since it was added in
       # rsync 3.1.0, but CentOS7 only has rsync 3.0.9. Anyway, we expect
       # everything in list.txt to be present (otherwise, tweak grep above).
       rsync -l --files-from=list.txt / ${VMCHECK_INSTTREE}/
       rm -f list.txt
    done

    make install DESTDIR=${VMCHECK_INSTTREE}
    vm_rsync

    vm_cmd env INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/sync.sh
    exit 0
else

    # then do this in the VM
    set -x
    ostree admin unlock || :

    # Now, overlay our built binaries & config files
    INSTTREE=/var/roothome/sync/insttree
    rsync -rlv $INSTTREE/usr/ /usr/
    if [ -d $INSTTREE/etc ]; then # on CentOS, the dbus service file is in /usr
      rsync -rlv $INSTTREE/etc/ /etc/
    fi

    restorecon -v /usr/bin/rpm-ostree
    restorecon -v /usr/libexec/rpm-ostreed
    mkdir -p /etc/systemd/system/rpm-ostreed.service.d
    # For our test suite at least, to catch things like https://github.com/projectatomic/rpm-ostree/issues/826
    cat > /etc/systemd/system/rpm-ostreed.service.d/fatal-warnings.conf << EOF
[Service]
Environment=G_DEBUG=fatal-warnings
EOF
    systemctl daemon-reload
    systemctl restart rpm-ostreed
fi
