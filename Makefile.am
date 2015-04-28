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

SUBDIRS += .

if ENABLE_GTK_DOC
SUBDIRS += doc
endif

privdatadir=$(pkglibdir)

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" \
        -DRPM_OSTREE_FEATURES='"$(RPM_OSTREE_FEATURES)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_40_
AM_CFLAGS += $(WARN_CFLAGS)

EXTRA_DIST += autogen.sh COPYING

SED_SUBST = sed \
        -e 's,[@]libexecdir[@],$(libexecdir),g' \
        $(NULL)

libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(PKGDEP_GIO_UNIX_CFLAGS) -I$(libglnx_srcpath) -fvisibility=hidden
libglnx_ldflags :=
libglnx_libs := $(PKGDEP_GIO_UNIX_LIBS)

include $(INTROSPECTION_MAKEFILE)
GIRS =
TYPELIBS = $(GIRS:.gir=.typelib)

include libglnx/Makefile-libglnx.am
noinst_LTLIBRARIES += libglnx.la
include Makefile-libpriv.am
include Makefile-lib.am
include Makefile-daemon.am
include Makefile-rpm-ostree.am
include Makefile-tests.am
include Makefile-man.am
