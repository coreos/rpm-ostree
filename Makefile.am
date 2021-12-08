# Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

include Makefile-decls.am
include Makefile-extra.inc

primaryname = rpm-ostree

SUBDIRS += .

if ENABLE_GTK_DOC
SUBDIRS += api-doc
endif

RPM_OSTREE_GITREV=$(shell if command -v git >/dev/null 2>&1 && test -e $(srcdir)/.git; then git describe --abbrev=42 --tags --always HEAD; fi)

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" \
	-DSYSCONFDIR='"$(sysconfdir)"' \
	-DRPM_OSTREE_GITREV='"$(RPM_OSTREE_GITREV)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_56
# Compiler warnings
warnings_skipped = sign-compare
warnings_error = empty-body format=2 format-security format-nonliteral \
  missing-declarations return-type switch overflow undef \
  misleading-indentation missing-include-dirs unused-result \
  $(NULL)
warning_flags = -Wall \
  $(patsubst %,-Wno-%,$(warnings_skipped)) \
  $(patsubst %,-Werror=%,$(warnings_error)) \
  $(NULL)
if BUILDOPT_WERROR
warning_flags += -Werror
endif
# We make some C warnings always a hard error; these should never happen in our code.
# These flags are not valid for C++.
warnings_error_only_c = strict-prototypes missing-prototypes \
  implicit-function-declaration int-conversion incompatible-pointer-types \
  $(NULL)
sanitizer_flags =
if BUILDOPT_ASAN
sanitizer_flags += -fsanitize=address -fsanitize=undefined -fsanitize-undefined-trap-on-error
endif
# See the AM_CFLAGS in libostree for more information about -fno-strict-aliasing
AM_CFLAGS += -std=gnu11 -fno-strict-aliasing $(warning_flags) $(patsubst %,-Werror=%,$(warnings_error_only_c))
# Our default CXX flags
AM_CXXFLAGS += -std=c++17 -fno-strict-aliasing $(warning_flags) $(sanitizer_flags)

EXTRA_DIST += autogen.sh COPYING

SED_SUBST = sed \
        -e 's,[@]libexecdir[@],$(libexecdir),g' \
        -e 's,[@]primaryname[@],$(primaryname),g' \
        -e 's,[@]bindir[@],$(bindir),g' \
        $(NULL)

libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(PKGDEP_GIO_UNIX_CFLAGS) -I$(libglnx_srcpath) -fvisibility=hidden
libglnx_ldflags :=
libglnx_libs := $(PKGDEP_GIO_UNIX_LIBS)

include $(INTROSPECTION_MAKEFILE)
GIRS =
TYPELIBS = $(GIRS:.gir=.typelib)

# These bits were originally based on gnome:librsvg/Makefile.am
# but have been adapted to non-recursive Automake, and additionally
# the "cargo vendor" bits for us live in packaging/Makefile.dist-packaging.
.PHONY: check-local-cargo clean-local-cargo use-git-not-dist-hook
if RUST_DEBUG
CARGO_RELEASE_ARGS=
else
CARGO_RELEASE_ARGS=--release
endif

check-local-cargo:
	cargo test
CHECK_LOCAL_HOOKS += check-local-cargo

clean-local-cargo:
	cargo clean
CLEAN_LOCAL_HOOKS += clean-local-cargo

use-git-not-dist-hook:
	@echo
	@echo 'ERROR: rpm-ostree does not use/support "make dist"; see packaging/Makefile.dist-packaging' 1>&2
	@echo
	@exit 1

DIST_HOOKS += use-git-not-dist-hook

include libglnx/Makefile-libglnx.am.inc
noinst_LTLIBRARIES += libglnx.la
include Makefile-libpriv.am
include Makefile-lib.am
include Makefile-daemon.am
include Makefile-rpm-ostree.am
include Makefile-tests.am
include Makefile-man.am
include Makefile-bash.am

-include $(top_srcdir)/git.mk
