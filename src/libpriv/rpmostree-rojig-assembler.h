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
#include "rpmostree-rojig-core.h"

G_BEGIN_DECLS

typedef struct RpmOstreeRojigAssembler RpmOstreeRojigAssembler;

#define RPMOSTREE_TYPE_ROJIG_ASSEMBLER         (rpmostree_rojig_assembler_get_type ())
#define RPMOSTREE_ROJIG_ASSEMBLER(inst)        (G_TYPE_CHECK_INSTANCE_CAST ((inst), RPMOSTREE_TYPE_ROJIG_ASSEMBLER, RpmOstreeRojigAssembler))
#define RPMOSTREE_IS_ROJIG_ASSEMBLER(inst)     (G_TYPE_CHECK_INSTANCE_TYPE ((inst), RPMOSTREE_TYPE_ROJIG_ASSEMBLER))

GType rpmostree_rojig_assembler_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeRojigAssembler, g_object_unref)

RpmOstreeRojigAssembler*
rpmostree_rojig_assembler_new_take_fd (int *fd,
                                       DnfPackage *pkg, /* for metadata */
                                       GError **error);

gboolean
rpmostree_rojig_assembler_read_meta (RpmOstreeRojigAssembler    *rojig,
                                     char             **out_checksum,
                                     GVariant         **commit,
                                     GVariant         **detached_meta,
                                     GCancellable      *cancellable,
                                     GError           **error);

gboolean
rpmostree_rojig_assembler_write_new_objects (RpmOstreeRojigAssembler    *rojig,
                                             OstreeRepo        *repo,
                                             GCancellable      *cancellable,
                                             GError           **error);


GVariant * rpmostree_rojig_assembler_get_xattr_table (RpmOstreeRojigAssembler *self);

gboolean
rpmostree_rojig_assembler_next_xattrs (RpmOstreeRojigAssembler    *self,
                                       GVariant         **out_objid_to_xattrs,
                                       GCancellable      *cancellable,
                                       GError           **error);

gboolean
rpmostree_rojig_assembler_xattr_lookup (GVariant *xattr_table,
                                        const char *path,
                                        GVariant *xattrs,
                                        GVariant **out_xattrs,
                                        GError  **error);

G_END_DECLS
