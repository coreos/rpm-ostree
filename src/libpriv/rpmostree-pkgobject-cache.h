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

typedef struct RpmOstreePkgObjectCache RpmOstreePkgObjectCache;

RpmOstreePkgObjectCache * _rpmostree_pkg_object_cache_new (void);

gboolean
_rpmostree_pkg_object_cache_load_source (RpmOstreePkgObjectCache *cache,
                                         OstreeRepo              *repo,
                                         const char              *commit,
                                         GCancellable            *cancellable,
                                         GError                 **error);

gboolean
_rpmostree_pkg_object_cache_load_target (RpmOstreePkgObjectCache *cache,
                                         int                      dfd,
                                         GCancellable            *cancellable,
                                         GError                 **error);

const char *
_rpmostree_pkg_object_cache_query (RpmOstreePkgObjectCache *cache,
                                   const char              *filename);

void
_rpmostree_pkg_object_cache_free (RpmOstreePkgObjectCache *cache);

