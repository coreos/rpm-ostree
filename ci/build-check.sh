#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# Add checks here which depend on the build container
# but don't require a full build (code static analysis).
if test -x /usr/bin/rustfmt; then
    echo "Verifying rustfmt"
    if !git diff --quiet --exit-code; then
        echo "outstanding diff before rustfmt" 1>&2
        exit 1
    fi
    make -f Makefile-extra.inc rustfmt
    if git diff --quiet --exit-code; then
        git diff
        echo "Please run rustfmt"
        exit 1
    fi
else
    echo "No /usr/bin/rustfmt, skipping"
fi

${dn}/build.sh
# NB: avoid make function because our RPM building doesn't
# support parallel runs right now
# See https://github.com/containers/fuse-overlayfs/pull/105 for the fuse-overlayfs workaround
if ! [ "$(findmnt -n -o SOURCE /)" != fuse-overlayfs ]; then
    /usr/bin/make check
fi
make rust-test
make install

# And now a clang build with -Werror turned on. We can't do this with gcc (in
# build.sh) because it doesn't support -Wno-error=macro-redefined, (and neither
# does clang on CentOS). Anyway, all we want is at least one clang run.
if test -x /usr/bin/clang; then
  if grep -q -e 'static inline.*_GLIB_AUTOPTR_LIST_FUNC_NAME' /usr/include/glib-2.0/glib/gmacros.h; then
      echo 'Skipping clang check, see https://bugzilla.gnome.org/show_bug.cgi?id=796346'
  else
  # Except unused-command-line-argument:
  #   error: argument unused during compilation: '-specs=/usr/lib/rpm/redhat/redhat-hardened    -cc1' [-Werror,-Wunused-command-line-argument]
  # Except for macro-redefined:
  #   /usr/include/python2.7/pyconfig-64.h:1199:9: error: '_POSIX_C_SOURCE' macro redefined
  # Except for deprecated-declarations: libdnf python bindings uses deprecated
  # functions
  export CFLAGS="-Wall -Werror -Wno-error=deprecated-declarations -Wno-error=macro-redefined -Wno-error=unused-command-line-argument ${CFLAGS:-}"
  export CC=clang
  git clean -dfx && git submodule foreach git clean -dfx
  # XXX: --disable-introspection because right now we're always building the
  # introspection bits with gcc, which doesn't understand some of the flags
  # above (see Makefile-lib.am)
  build ${CONFIGOPTS:-} --disable-introspection
  fi
fi
