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

newname = nts
if BUILDOPT_NEW_NAME
primaryname = $(newname)
else
primaryname = rpm-ostree
endif

SUBDIRS += .

if ENABLE_GTK_DOC
SUBDIRS += api-doc
endif

RPM_OSTREE_GITREV=$(shell if command -v git >/dev/null 2>&1 && test -e $(srcdir)/.git; then git describe --abbrev=42 --tags --always HEAD; fi)

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
# WITH_SWDB is a hackaround for it being used in libdnf headers
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' \
	-DWITH_SWDB=0 \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" \
	-DSYSCONFDIR='"$(sysconfdir)"' \
	-DRPM_OSTREE_FEATURES='"$(RPM_OSTREE_FEATURES)"' \
	-DRPM_OSTREE_GITREV='"$(RPM_OSTREE_GITREV)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_50 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_50
# Keep this in sync with the AM_CFLAGS in libostree; see
# that project for more information about e.g. -fno-strict-aliasing
AM_CFLAGS += -std=gnu11 -fno-strict-aliasing $(WARN_CFLAGS)
# bundled libdnf
AM_LDFLAGS += "-Wl,-rpath=$(libdir)/rpm-ostree"

EXTRA_DIST += autogen.sh COPYING

GITIGNOREFILES += build-aux/ gtk-doc.make config.h.in aclocal.m4 insttree/ rpm-ostreed

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
	cd $(srcdir)/rust && CARGO_TARGET_DIR=$(abs_top_builddir)/target cargo test
CHECK_LOCAL_HOOKS += check-local-cargo

clean-local-cargo:
	cd $(srcdir)/rust && CARGO_TARGET_DIR=$(abs_top_builddir)/target cargo clean
CLEAN_LOCAL_HOOKS += clean-local-cargo

use-git-not-dist-hook:
	@echo
	@echo 'ERROR: rpm-ostree does not use/support "make dist"; see packaging/Makefile.dist-packaging' 1>&2
	@echo
	@exit 1

DIST_HOOKS += use-git-not-dist-hook
GITIGNOREFILES += target/

include libglnx/Makefile-libglnx.am.inc
noinst_LTLIBRARIES += libglnx.la
include Makefile-libpriv.am
include Makefile-libdnf.am
include Makefile-lib.am
include Makefile-daemon.am
include Makefile-rpm-ostree.am
include Makefile-tests.am
include Makefile-man.am

-include $(top_srcdir)/git.mk
