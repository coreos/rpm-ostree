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

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS = -DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_34 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_34
AM_CFLAGS = $(WARN_CFLAGS)
DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc --disable-maintainer-mode

# Subdirectories
SUBDIRS = .

#if ENABLE_GTK_DOC
#SUBDIRS += doc
#endif

# Common variables
NULL = 
INSTALL_DATA_HOOKS =
BUILT_SOURCES =
MANPAGES =
CLEANFILES =
EXTRA_DIST =
bin_PROGRAMS =
sbin_PROGRAMS =
bin_SCRIPTS =
lib_LTLIBRARIES =
libexec_PROGRAMS =
noinst_LTLIBRARIES =
noinst_PROGRAMS =
privlibdir = $(pkglibdir)
privlib_LTLIBRARIES =
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =
INTROSPECTION_GIRS = 
girdir = $(datadir)/gir-1.0
gir_DATA =
typelibdir = $(libdir)/girepository-1.0
typelib_DATA =

EXTRA_DIST += autogen.sh COPYING

include Makefile-rpm-ostree.am
