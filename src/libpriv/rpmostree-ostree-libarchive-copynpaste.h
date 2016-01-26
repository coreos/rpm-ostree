/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <ostree.h>
#include <archive.h>
#include <archive_entry.h>

#include "libglnx.h"

gboolean
rpmostree_split_path_ptrarray_validate (const char *path,
                                        GPtrArray  **out_components,
                                        GError     **error);

GFileInfo *_rpmostree_libarchive_to_file_info (struct archive_entry *entry);

gboolean
_rpmostree_import_libarchive_entry_file (OstreeRepo           *repo,
                                         struct archive       *a,
                                         struct archive_entry *entry,
                                         GFileInfo            *file_info,
                                         GVariant             *xattrs,
                                         guchar              **out_csum,
                                         GCancellable         *cancellable,
                                         GError              **error);
