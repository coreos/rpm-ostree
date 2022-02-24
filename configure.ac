AC_PREREQ([2.63])
dnl
dnl SEE RELEASE.md FOR INSTRUCTIONS ON HOW TO DO A RELEASE.
dnl
m4_define([year_version], [2022])
m4_define([release_version], [3])
m4_define([package_version], [year_version.release_version])
AC_INIT([rpm-ostree], [package_version], [coreos@lists.fedoraproject.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([buildutil])
AC_CONFIG_AUX_DIR([build-aux])
dnl Versioning information
AC_SUBST([YEAR_VERSION], [year_version])
AC_SUBST([RELEASE_VERSION], [release_version])
AC_SUBST([PACKAGE_VERSION], [package_version])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz subdir-objects])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
dnl we don't today use C++ in rpm-ostree, only in libdnf,
dnl but this has the side effect of setting CXXFLAGS with -g
dnl if not set, which we definitely want; cmake doesn't do that.
AC_PROG_CXX
AM_PROG_CC_C_O

AC_MSG_CHECKING(whether to build in debug mode)
debug_release=release
if $(echo $CFLAGS |grep -q -E "(-O0|-Og)"); then
  debug_release=debug
fi
AC_MSG_RESULT($debug_release)

dnl These bits based on gnome:librsvg/configure.ac
dnl By default, we build in release mode (but without LTO!)
AC_ARG_ENABLE(rust-debug,
  AC_HELP_STRING([--enable-rust-debug],
    [Build Rust code with debugging information [default=no]]),
    [rust_debug_release=$enableval],
    [rust_debug_release=$debug_release])
dnl Canonicalize yes/no to debug/release
AS_IF([test x$rust_debug_release = xno ], [rust_debug_release=release])
AS_IF([test x$rust_debug_release = xyes ], [rust_debug_release=debug])

AC_ARG_ENABLE(sanitizers,
              AS_HELP_STRING([--enable-sanitizers],
                             [Enable ASAN and UBSAN (default: no)]),,
              [enable_sanitizers=no])
AM_CONDITIONAL(BUILDOPT_ASAN, [test x$enable_sanitizers != xno])

AC_ARG_ENABLE(bin-unit-tests,
              AS_HELP_STRING([--enable-bin-unit-tests],
                             [(default: yes if debug build, no for release build)]),,
              [enable_bin_unit_tests=maybe])
case "${enable_bin_unit_tests}-${debug_release}" in
  maybe-debug) enable_bin_unit_tests=yes;;
  maybe-*) enable_bin_unit_tests=no;;
esac
  
AS_IF([test x$enable_bin_unit_tests = xyes], [
  AC_DEFINE([BUILDOPT_BIN_UNIT_TESTS], 1, [Define if unit tests are injected into the binary])
])
AM_CONDITIONAL(BUILDOPT_BIN_UNIT_TESTS, [test x$enable_bin_unit_tests = xyes])

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])


PKG_PROG_PKG_CONFIG

dnl Remember to update AM_CPPFLAGS in Makefile.am when bumping GIO req.
PKG_CHECK_MODULES(PKGDEP_GIO_UNIX, [gio-unix-2.0])
dnl These are the dependencies of the public librpmostree-1.0.0 shared library
PKG_CHECK_MODULES(PKGDEP_LIBRPMOSTREE, [gio-unix-2.0 >= 2.50.0 json-glib-1.0 ostree-1 >= 2021.2 rpm])
dnl And these additional ones are used by for the rpmostreeinternals C/C++ library
PKG_CHECK_MODULES(PKGDEP_RPMOSTREE, [polkit-gobject-1 libarchive])

dnl RHEL8.1 has old libarchive
AS_IF([pkg-config --atleast-version=3.3.3 libarchive],
  [AC_DEFINE([HAVE_LIBARCHIVE_ZSTD], 1, [Define if we have libarchive with zstd])])

# rpmver is available in rpm >= 4.16, el8 is still on 4.14
AC_SEARCH_LIBS([rpmverCmp], [rpm],
  AC_DEFINE([BUILDOPT_HAVE_RPMVER], 1, [Whether rpmver API is available in librpmio]),
  AC_DEFINE([BUILDOPT_HAVE_RPMVER], 0, [Whether rpmver API is available in librpmio])
)

dnl We don't *actually* use this ourself, but librepo does, and libdnf gets confused
dnl if librepo doesn't support it.
have_zchunk=no
AS_IF([pkg-config --exists zck],
  [have_zchunk=yes; AC_DEFINE([HAVE_ZCHUNK], 1, [Define if we have zchunk])])

AC_PATH_PROG([XSLTPROC], [xsltproc])

GLIB_TESTS
LIBGLNX_CONFIGURE

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.34.0])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
],[
AM_CONDITIONAL([ENABLE_GTK_DOC],[false])
])

AC_ARG_ENABLE(werror,
              AS_HELP_STRING([--enable-werror],
                             [Enable -Werror for C/C++ (default: no)]),,
              [enable_werror=no])
AM_CONDITIONAL(BUILDOPT_WERROR, [test x$enable_werror != xno])

PKG_CHECK_VAR(BASH_COMPLETIONSDIR, [bash-completion], [completionsdir],,
  BASH_COMPLETIONSDIR="${datadir}/bash-completion/completions")
AC_SUBST(BASH_COMPLETIONSDIR)

AC_PATH_PROG([cargo], [cargo])
AS_IF([test -z "$cargo"], [AC_MSG_ERROR([cargo is required for --enable-rust])])
AC_PATH_PROG([rustc], [rustc])
AS_IF([test -z "$rustc"], [AC_MSG_ERROR([rustc is required for --enable-rust])])

dnl See comment in installdeps.sh
AM_CONDITIONAL(BUILDOPT_PREBUILT_BINDINGS, [test -f rpmostree-cxxrs-prebuilt.h])


RUST_TARGET_SUBDIR=${rust_debug_release}
AC_SUBST([RUST_TARGET_SUBDIR])
AM_CONDITIONAL(RUST_DEBUG, [test "x$rust_debug_release" = "xdebug"])

AC_CONFIG_FILES([
Makefile
api-doc/Makefile
src/lib/rpm-ostree-1.pc
src/lib/rpmostree-version.h
])
AC_OUTPUT

echo "
    $PACKAGE $VERSION

    introspection:                           $found_introspection
    ASAN + UBSAN:                            ${enable_sanitizers:-no}
    gtk-doc:                                 $enable_gtk_doc
    rust:                                    $rust_debug_release
    bdb rpmdb default:                       ${enable_bdb_rpmdb_default}
"
