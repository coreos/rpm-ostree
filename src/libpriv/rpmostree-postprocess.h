/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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
#include "rpmostree-json-parsing.h"

gboolean
rpmostree_treefile_postprocessing (GFile         *rootfs,
                                   GFile         *context_directory,
                                   GBytes        *serialized_treefile,
                                   JsonObject    *treefile,
                                   GCancellable  *cancellable,
                                   GError       **error);

gboolean
rpmostree_rootfs_postprocess_common (int           rootfs_fd,
                                     GCancellable *cancellable,
                                     GError       **error);

gboolean
rpmostree_prepare_rootfs_get_sepolicy (int            dfd,
                                       const char    *path,
                                       OstreeSePolicy **out_sepolicy,
                                       GCancellable  *cancellable,
                                       GError       **error);

gboolean
rpmostree_prepare_rootfs_for_commit (GFile         *rootfs,
                                     JsonObject    *treefile,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
rpmostree_commit (int            rootfs_dfd,
                  OstreeRepo    *repo,
                  const char    *refname,
                  GVariant      *metadata,
                  const char    *gpg_keyid,
                  gboolean       enable_selinux,
                  OstreeRepoDevInoCache *devino_cache,
                  char         **out_new_revision,
                  GCancellable  *cancellable,
                  GError       **error);
gboolean
rpmostree_copy_additional_files (GFile         *rootfs,
                                 GFile         *context_directory,
                                 JsonObject    *treefile,
                                 GCancellable  *cancellable,
                                 GError       **error);
