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

#include "libglnx.h"
#include "rpmostree-jigdo-core.h"

typedef struct RpmOstreeJigdoAssembler RpmOstreeJigdoAssembler;

#define RPMOSTREE_TYPE_JIGDO_ASSEMBLER         (rpmostree_jigdo_assembler_get_type ())
#define RPMOSTREE_JIGDO_ASSEMBLER(inst)        (G_TYPE_CHECK_INSTANCE_CAST ((inst), RPMOSTREE_TYPE_JIGDO_ASSEMBLER, RpmOstreeJigdoAssembler))
#define RPMOSTREE_IS_JIGDO_ASSEMBLER(inst)     (G_TYPE_CHECK_INSTANCE_TYPE ((inst), RPMOSTREE_TYPE_JIGDO_ASSEMBLER))

GType rpmostree_jigdo_assembler_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeJigdoAssembler, g_object_unref)

RpmOstreeJigdoAssembler*
rpmostree_jigdo_assembler_new_take_fd (int *fd,
                                       DnfPackage *pkg, /* for metadata */
                                       GError **error);

gboolean
rpmostree_jigdo_assembler_read_meta (RpmOstreeJigdoAssembler    *jigdo,
                                     char             **out_checksum,
                                     GVariant         **commit,
                                     GVariant         **detached_meta,
                                     GVariant         **pkgs,
                                     GCancellable      *cancellable,
                                     GError           **error);

gboolean
rpmostree_jigdo_assembler_write_new_objects (RpmOstreeJigdoAssembler    *jigdo,
                                             OstreeRepo        *repo,
                                             GCancellable      *cancellable,
                                             GError           **error);


GVariant * rpmostree_jigdo_assembler_get_xattr_table (RpmOstreeJigdoAssembler *self);

gboolean
rpmostree_jigdo_assembler_next_xattrs (RpmOstreeJigdoAssembler    *self,
                                       GVariant         **out_objid_to_xattrs,
                                       GCancellable      *cancellable,
                                       GError           **error);

gboolean
rpmostree_jigdo_assembler_xattr_lookup (GVariant *xattr_table,
                                        const char *path,
                                        GVariant *xattrs,
                                        GVariant **out_xattrs,
                                        GError  **error);
