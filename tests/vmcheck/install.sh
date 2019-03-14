#!/bin/bash
set -euo pipefail

# This is just a small wrapper for `make install`, but with the added logic to
# pull in ostree packages from the build container. We always assume development
# and testing is against git master ostree and that the build container is
# tracking e.g. CAHC or FAHC (see HACKING.md for more details).

DESTDIR=${topsrcdir}/insttree

# Chown everything to writable, due to
# https://bugzilla.redhat.com/show_bug.cgi?id=517575
if test -d ${DESTDIR}; then chmod -R u+w ${DESTDIR}/; fi
rm -rf ${DESTDIR}
mkdir -p ${DESTDIR}

ostree --version
# We don't want to sync all of userspace, just things
# that rpm-ostree links to or uses and tend to drift
# in important ways.
# XXX: We add libmodulemd manually for now until it's
# part of the image.
pkgs="libsolv libmodulemd1"
if rpm -q zchunk-libs 2>/dev/null; then
    pkgs="${pkgs} zchunk-libs"
fi
for pkg in ostree{,-libs,-grub2} ${pkgs}; do

    rpm -q $pkg

    # We do not have perms to read /etc/grub2 as non-root. In the prebuilt
    # container case, manpages are missing. Ignore that.
    rpm -ql $pkg | grep -vE "^/(etc|usr/share/(doc|man))/" >  list.txt

    # See above chown https://bugzilla.redhat.com/show_bug.cgi?id=517575
    chmod -R u+w ${DESTDIR}/

    # Note we cant use --ignore-missing-args here since it was added in
    # rsync 3.1.0, but CentOS7 only has rsync 3.0.9. Anyway, we expect
    # everything in list.txt to be present (otherwise, tweak grep above).
    rsync -l --files-from=list.txt / ${DESTDIR}/

    rm -f list.txt
done

make install DESTDIR=${DESTDIR}
