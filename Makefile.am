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

privdatadir=$(pkglibdir)

ACLOCAL_AMFLAGS += -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_36 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_36
AM_CFLAGS += $(WARN_CFLAGS)

EXTRA_DIST += autogen.sh COPYING

libgsystem_srcpath := libgsystem
libgsystem_cflags = $(PKGDEP_GIO_UNIX_CFLAGS) -I$(srcdir)/libgsystem
libgsystem_libs = $(PKGDEP_GIO_UNIX_LIBS)
include libgsystem/Makefile-libgsystem.am
privlib_LTLIBRARIES += libgsystem.la

include $(INTROSPECTION_MAKEFILE)

include Makefile-rpm-ostree.am
include Makefile-gsystem-introspection.am

girdir = $(pkgdatadir)/gir-1.0
typelibdir = $(pkglibdir)/girepository-1.0

gir_DATA = $(INTROSPECTION_GIRS)
typelib_DATA = $(gir_DATA:.gir=.typelib)

include Makefile-autobuilder.am
