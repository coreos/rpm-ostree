#!/bin/sh
set -e

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

olddir=`pwd`
cd $srcdir

if ! which autoreconf 2>/dev/null; then
        echo "*** No autoreconf found, please install it ***"
        exit 1
fi

mkdir -p m4

if ! which gtkdocize 2>/dev/null; then
        echo "You don't have gtk-doc installed, and thus won't be able to generate the documentation."
        rm -f gtk-doc.make
        cat > gtk-doc.make <<EOF
EXTRA_DIST =
CLEANFILES =
EOF
else
        gtkdocize || exit $?
fi

if ! test -f libglnx/README.md; then
    git submodule update --init
fi
# Workaround automake bug with subdir-objects and computed paths
sed -e 's,$(libglnx_srcpath),'${srcdir}/libglnx,g < libglnx/Makefile-libglnx.am >libglnx/Makefile-libglnx.am.inc


autoreconf --force --install --verbose

cd $olddir
test -n "$NOCONFIGURE" || "$srcdir/configure" "$@"
