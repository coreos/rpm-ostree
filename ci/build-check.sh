#!/usr/bin/bash
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
${dn}/build.sh
# NB: avoid make function because our RPM building doesn't
# support parallel runs right now
/usr/bin/make check
make install

# And now a clang build with -Werror turned on. We can't do this with gcc (in
# build.sh) because it doesn't support -Wno-error=macro-redefined, (and neither
# does clang on CentOS). Anyway, all we want is at least one clang run.
if test -x /usr/bin/clang; then
  # Except unused-command-line-argument:
  #   error: argument unused during compilation: '-specs=/usr/lib/rpm/redhat/redhat-hardened    -cc1' [-Werror,-Wunused-command-line-argument]
  # Except for macro-redefined:
  #   /usr/include/python2.7/pyconfig-64.h:1199:9: error: '_POSIX_C_SOURCE' macro redefined
  # Except for deprecated-declarations: libdnf python bindings uses deprecated
  # functions
  export CFLAGS="-Wall -Werror -Wno-error=deprecated-declarations -Wno-error=macro-redefined -Wno-error=unused-command-line-argument ${CFLAGS:-}"
  export CC=clang
  git clean -dfx && git submodule foreach git clean -dfx
  build ${CONFIGOPTS:-}
fi
