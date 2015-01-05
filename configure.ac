AC_PREREQ([2.63])
AC_INIT([rpm-ostree], [2014.114], [walters@verbum.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([buildutil])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O

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

PKG_CHECK_MODULES(PKGDEP_GIO_UNIX, [gio-unix-2.0])
PKG_CHECK_MODULES(PKGDEP_RPMOSTREE, [gio-unix-2.0 json-glib-1.0
				     ostree-1 >= 2014.13 libgsystem >= 2014.3
				     rpm hawkey])
AC_PATH_PROG([XSLTPROC], [xsltproc])

GLIB_TESTS

GOBJECT_INTROSPECTION_REQUIRE([1.34.0])

AC_ARG_ENABLE(installed_tests,
              AS_HELP_STRING([--enable-installed-tests],
                             [Install test programs (default: no)]),,
              [enable_installed_tests=no])
AM_CONDITIONAL(BUILDOPT_INSTALL_TESTS, test x$enable_installed_tests = xyes)

AC_ARG_ENABLE(patched-hawkey-and-libsolv,
              AS_HELP_STRING([--enable-patched-hawkey],
                             [Assert that our copy of hawkey and libsolv understand /usr/share/rpm]),,
              [enable_patched_hawkey_and_libsolv=no])
AM_CONDITIONAL(BUILDOPT_PATCHED_HAWKEY_AND_LIBSOLV, [test x$enable_patched_hawkey_and_libsolv = xyes])
AS_IF([test x$enable_patched_hawkey_and_libsolv = xyes], [
    AC_DEFINE(HAVE_PATCHED_HAWKEY_AND_LIBSOLV, 1, [Define if we have patched hawkey and libsolv])
])
if test x$enable_patched_hawkey_and_libsolv != xno; then RPM_OSTREE_FEATURES="$RPM_OSTREE_FEATURES +packaging"; fi

AC_ARG_ENABLE(usrbinatomic,
              AS_HELP_STRING([--enable-usrbinatomic],
                             [Alias client binary as /usr/bin/atomic]),,
              [enable_usrbinatomic=no])
AM_CONDITIONAL(BUILDOPT_USRBINATOMIC, [test x$enable_usrbinatomic = xyes])

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

AC_CONFIG_FILES([
Makefile
])
AC_OUTPUT

echo "
    $PACKAGE $VERSION

    patched hawkey:	$enable_patched_hawkey_and_libsolv
    usrbinatomic:	$enable_usrbinatomic
    compose tooling:	$enable_compose_tooling
"
