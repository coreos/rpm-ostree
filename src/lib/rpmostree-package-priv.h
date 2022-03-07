/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Red Hat, In.c
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#pragma once

#include "rpmostree-package.h"
#include <ostree.h>

G_BEGIN_DECLS

RpmOstreePackage *_rpm_ostree_package_new_from_variant (GVariant *gv_nevra);

gboolean _rpm_ostree_package_variant_list_for_commit (OstreeRepo *repo, const char *rev,
                                                      gboolean allow_noent, GVariant **out_pkglist,
                                                      GCancellable *cancellable, GError **error);

gboolean _rpm_ostree_package_list_for_commit (OstreeRepo *repo, const char *rev,
                                              gboolean allow_noent, GPtrArray **out_pkglist,
                                              GCancellable *cancellable, GError **error);
gboolean _rpm_ostree_diff_package_lists (GPtrArray *a, GPtrArray *b, GPtrArray **out_unique_a,
                                         GPtrArray **out_unique_b, GPtrArray **out_modified_a,
                                         GPtrArray **out_modified_b, GPtrArray **out_common);

G_END_DECLS
