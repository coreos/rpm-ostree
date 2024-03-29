# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

abs_top_builddir = @abs_top_builddir@

include ../Makefile-lib-defines.am

NULL =

# We require automake 1.6 at least.
AUTOMAKE_OPTIONS = 1.6

# The name of the module, e.g. 'glib'.
DOC_MODULE=rpmostree

# The top-level SGML file. You can change this if you want to.
DOC_MAIN_SGML_FILE=$(DOC_MODULE)-docs.xml

# The directory containing the source code. Relative to $(srcdir).
# gtk-doc will search all .c & .h files beneath here for inline comments
# documenting the functions and macros.
# e.g. DOC_SOURCE_DIR=../../../gtk
DOC_SOURCE_DIR=$(top_srcdir)/src/lib

# Extra options to pass to gtkdoc-scangobj. Not normally needed.
SCANGOBJ_OPTIONS= --version

# Extra options to supply to gtkdoc-scan.
# e.g. SCAN_OPTIONS=--deprecated-guards="GTK_DISABLE_DEPRECATED"
SCAN_OPTIONS= --rebuild-types

# Extra options to supply to gtkdoc-mkdb.
# e.g. MKDB_OPTIONS=--sgml-mode --output-format=xml
MKDB_OPTIONS=--sgml-mode --output-format=xml

# Extra options to supply to gtkdoc-mktmpl
# e.g. MKTMPL_OPTIONS=--only-section-tmpl
MKTMPL_OPTIONS=

# MKHTML_OPTIONS=--path="$(builddir)/html $(srcdir)/html"

# Extra options to supply to gtkdoc-fixref. Not normally needed.
# e.g. FIXXREF_OPTIONS=--extra-dir=../gdk-pixbuf/html --extra-dir=../gdk/html
FIXXREF_OPTIONS=

# Used for dependencies. The docs will be rebuilt if any of these change.
# e.g. HFILE_GLOB=$(top_srcdir)/gtk/*.h
# e.g. CFILE_GLOB=$(top_srcdir)/gtk/*.c
HFILE_GLOB=$(addprefix $(top_srcdir)/,$(librpmostree_public_headers))
CFILE_GLOB=$(top_srcdir)/src/lib/*.c

# Header files to ignore when scanning.
# e.g. IGNORE_HFILES=gtkdebug.h gtkintl.h
IGNORE_HFILES= \
	$(NULL)

# Images to copy into HTML directory.
# e.g. HTML_IMAGES=$(top_srcdir)/gtk/stock-icons/stock_about_24.png
HTML_IMAGES=

# Extra SGML files that are included by $(DOC_MAIN_SGML_FILE).
# e.g. content_files=running.sgml building.sgml changes-2.0.sgml
content_files= \
	overview.xml \
	$(NULL)

# SGML files where gtk-doc abbreviations (#GtkWidget) are expanded
# These files must be listed here *and* in content_files
# e.g. expand_content_files=running.sgml
expand_content_files= \
	version.xml \
	$(NULL)

# CFLAGS and LDFLAGS for compiling gtkdoc-scangobj with your library.
# Only needed if you are using gtkdoc-scangobj to dynamically query widget
# signals and properties.
# e.g. INCLUDES=-I$(top_srcdir) -I$(top_builddir) $(GTK_DEBUG_FLAGS)
# e.g. GTKDOC_LIBS=$(top_builddir)/gtk/$(gtktargetlib)
GTKDOC_LIBS=

# Hacks around gtk-doc brokenness for out of tree builds
rpmostree-sections.txt: $(srcdir)/rpmostree-sections.txt
	cp $< $@

$(DOC_MAIN_SGML_FILE): $(DOC_MAIN_SGML_FILE).in
	sed -e 's|\@SRCDIR\@|$(top_srcdir)|g' $< > $(@)

version.xml:
	echo -n $(VERSION) > "$@"

BUILT_SOURCES = $(DOC_MAIN_SGML_FILE)

# This includes the standard gtk-doc make rules, copied by gtkdocize.
include $(top_srcdir)/gtk-doc.make

CLEANFILES += $(BUILT_SOURCES) rpmostree-overrides.txt version.xml
EXTRA_DIST += $(DOC_MAIN_SGML_FILE).in

-include $(top_srcdir)/git.mk
