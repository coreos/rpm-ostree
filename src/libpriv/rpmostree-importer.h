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

#include "libglnx.h"
#include <rpm/rpmlib.h>
#include <libdnf/libdnf.h>

typedef struct RpmOstreeImporter RpmOstreeImporter;

#define RPMOSTREE_TYPE_IMPORTER         (rpmostree_importer_get_type ())
#define RPMOSTREE_IMPORTER(inst)        (G_TYPE_CHECK_INSTANCE_CAST ((inst), RPMOSTREE_TYPE_IMPORTER, RpmOstreeImporter))
#define RPMOSTREE_IS_IMPORTER(inst)     (G_TYPE_CHECK_INSTANCE_TYPE ((inst), RPMOSTREE_TYPE_IMPORTER))

GType rpmostree_importer_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeImporter, g_object_unref)

/**
 * RpmOstreeImporterFlags:
 * @RPMOSTREE_IMPORTER_FLAGS_SKIP_EXTRANEOUS: Skip files/directories outside of supported ostree-compliant paths rather than erroring out
 * @RPMOSTREE_IMPORTER_FLAGS_NODOCS: Skip documentation files
 */
typedef enum {
  RPMOSTREE_IMPORTER_FLAGS_SKIP_EXTRANEOUS =  (1 << 0),
  RPMOSTREE_IMPORTER_FLAGS_NODOCS =  (1 << 1),
} RpmOstreeImporterFlags;

RpmOstreeImporter*
rpmostree_importer_new_take_fd (int                     *fd,
                                OstreeRepo              *repo,
                                DnfPackage              *pkg,
                                RpmOstreeImporterFlags   flags,
                                OstreeSePolicy          *sepolicy,
                                GError                 **error);

void rpmostree_importer_set_jigdo_mode (RpmOstreeImporter *self,
                                        GVariant *xattr_table,
                                        GVariant *xattrs);

gboolean
rpmostree_importer_read_metainfo (int fd,
                                  Header *out_header,
                                  gsize *out_cpio_offset,
                                  rpmfi *out_fi,
                                  GError **error);

const char*
rpmostree_importer_get_ostree_branch (RpmOstreeImporter *unpacker);

gboolean
rpmostree_importer_run (RpmOstreeImporter *unpacker,
                        char             **out_commit,
                        GCancellable      *cancellable,
                        GError           **error);

void
rpmostree_importer_run_async (RpmOstreeImporter  *unpacker,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data);

char *
rpmostree_importer_run_async_finish (RpmOstreeImporter  *self,
                                     GAsyncResult       *res,
                                     GError            **error);

char *
rpmostree_importer_get_nevra (RpmOstreeImporter *self);

const char *
rpmostree_importer_get_header_sha256 (RpmOstreeImporter *self);
