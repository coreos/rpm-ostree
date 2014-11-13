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

typedef enum {
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_LEGACY,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW
} RpmOstreePostprocessBootLocation;

gboolean
rpmostree_treefile_postprocessing (GFile         *rootfs,
                                   GBytes        *serialized_treefile,
                                   JsonObject    *treefile,
                                   GCancellable  *cancellable,
                                   GError       **error);

gboolean
rpmostree_prepare_rootfs_for_commit (GFile         *rootfs,
                                     RpmOstreePostprocessBootLocation boot_style,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
rpmostree_commit (GFile         *rootfs,
                  OstreeRepo    *repo,
                  const char    *refname,
                  GVariant      *metadata,
                  const char    *gpg_keyid,
                  gboolean       enable_selinux,
                  GCancellable  *cancellable,
                  GError       **error);
