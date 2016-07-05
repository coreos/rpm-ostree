AC_PREREQ([2.63])
AC_INIT([rpm-ostree], [2016.4], [walters@verbum.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([buildutil])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz subdir-objects])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O

# dbus system dir
AC_MSG_CHECKING(for dbus system services directory)
if test "$enable_prefix_only" = "yes"; then
  dbusservicedir='${datadir}/dbus-1/system-services'
else
  dbusservicedir='${datadir}/dbus-1/system-services'
  if test "$dbusservicedir" = ""; then
    AC_MSG_ERROR(Couldn't find dbus services directory. Try installing dbus-devel)
  fi
fi
AC_SUBST([dbusservicedir], [$dbusservicedir])
AC_MSG_RESULT(dbusservicedir)

changequote(,)dnl
if test "x$GCC" = "xyes"; then
  WARN_CFLAGS="-Wall -Wstrict-prototypes -Werror=missing-prototypes \
		-Werror=implicit-function-declaration \
		-Werror=pointer-arith -Werror=init-self -Werror=format=2 \
		-Werror=format-security \
		-Werror=missing-include-dirs -Werror=aggregate-return \
		-Werror=declaration-after-statement"
fi
changequote([,])dnl
AC_SUBST(WARN_CFLAGS)

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

RPM_OSTREE_FEATURES=""
AC_SUBST([RPM_OSTREE_FEATURES])

PKG_PROG_PKG_CONFIG

save_LIBS="$LIBS"
LIBS=
AC_SEARCH_LIBS([cap_init], [cap], [], [AC_MSG_ERROR([*** POSIX caps library not found])])
CAP_LIBS="$LIBS"
AC_SUBST(CAP_LIBS)
LIBS="$save_LIBS"

AC_SEARCH_LIBS([rpmsqSetInterruptSafety], [rpmio],
  AC_DEFINE([BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY], 1, [Set to 1 if we have interrupt safety API]),
  AC_DEFINE([BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY], 0, [Set to 1 if we have interrupt safety API])
)

# Remember to update AM_CPPFLAGS in Makefile.am when bumping GIO req.
PKG_CHECK_MODULES(PKGDEP_GIO_UNIX, [gio-unix-2.0])
PKG_CHECK_MODULES(PKGDEP_RPMOSTREE, [gio-unix-2.0 >= 2.40.0 json-glib-1.0
				     ostree-1 >= 2016.7 libgsystem >= 2015.1
				     libsystemd
				     rpm librepo
				     libarchive])
dnl bundled libhif
PKGDEP_RPMOSTREE_CFLAGS="-I $(pwd)/libhif -I $(pwd)/libhif-build $PKGDEP_RPMOSTREE_CFLAGS"
PKGDEP_RPMOSTREE_LIBS="-L$(pwd)/libhif-build/libhif -lhif $PKGDEP_RPMOSTREE_LIBS"

AC_PATH_PROG([XSLTPROC], [xsltproc])

GLIB_TESTS

AC_CHECK_TOOL(GPERF, gperf)
AS_IF([test -z "$GPERF"],
  AC_MSG_ERROR([*** gperf not found])
)

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.34.0])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
],[
AM_CONDITIONAL([ENABLE_GTK_DOC],[false])
])

AC_ARG_ENABLE(installed_tests,
              AS_HELP_STRING([--enable-installed-tests],
                             [Install test programs (default: no)]),,
              [enable_installed_tests=no])
AM_CONDITIONAL(BUILDOPT_INSTALL_TESTS, test x$enable_installed_tests = xyes)

AC_ARG_WITH(bubblewrap,
              AS_HELP_STRING([--with-bubblewrap],
                             [Path to bubblewrap binary (default: /usr/bin/bwrap)]),,
              [with_bubblewrap=/usr/bin/bwrap])
AC_DEFINE_UNQUOTED(WITH_BUBBLEWRAP_PATH, ["$with_bubblewrap"], [Define to bubblewrap path])

dnl Some distributions may want to support the client tooling, but not
dnl the server side.
AC_ARG_ENABLE(compose-tooling,
              AS_HELP_STRING([--enable-compose-tooling],
                             [Build compose tooling]),,
              [enable_compose_tooling=yes])
AM_CONDITIONAL(BUILDOPT_COMPOSE_TOOLING, [test x$enable_compose_tooling = xyes])
AS_IF([test x$enable_compose_tooling = xyes], [
    AC_DEFINE(HAVE_COMPOSE_TOOLING, 1, [Define if we are building compose tooling])
])
if test x$enable_compose_tooling != xno; then RPM_OSTREE_FEATURES="$RPM_OSTREE_FEATURES +compose"; fi

dnl Try to automatically determine cmake type from CFLAGS
if $(echo $CFLAGS |grep -q -e "-O0"); then
  cmake_args="-DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS_DEBUG:STRING='-ggdb -O0' \
        -DCMAKE_CXX_FLAGS_DEBUG:STRING=-ggdb -O0'"
  export cmake_args
else
  cmake_args=-DCMAKE_BUILD_TYPE=RelWithDebugInfo
  export cmake_args
fi  

dnl I picked /usr/libexec/rpm-ostree just because we need an
dnl arbitrary path - we don't actually install there.
(set -euo pipefail; mkdir -p libhif-build && cd libhif-build &&
 set -x && 
 cmake \
     -DCMAKE_INSTALL_PREFIX:PATH=/usr/libexec/rpm-ostree \
     -DINCLUDE_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree/include \
     -DLIB_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree \
     -DSYSCONF_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree/etc \
     -DSHARE_INSTALL_PREFIX:PATH=/usr/libexec/rpm-ostree/share \
     -DBUILD_SHARED_LIBS:BOOL=ON \
     ${cmake_args} ../libhif) || exit 1

AC_CONFIG_FILES([
Makefile
api-doc/Makefile
src/lib/rpm-ostree-1.pc
])
AC_OUTPUT

echo "
    $PACKAGE $VERSION

    compose tooling:	$enable_compose_tooling
    bubblewrap:         $with_bubblewrap
    gtk-doc:            $enable_gtk_doc
"
