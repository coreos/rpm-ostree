/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
#include <rpmostree-package.h>

G_BEGIN_DECLS

_RPMOSTREE_EXTERN GPtrArray *rpm_ostree_db_query_all (OstreeRepo *repo, const char *ref,
                                                      GCancellable *cancellable, GError **error);

_RPMOSTREE_EXTERN gboolean rpm_ostree_db_diff (OstreeRepo *repo, const char *orig_ref,
                                               const char *new_ref, GPtrArray **out_removed,
                                               GPtrArray **out_added, GPtrArray **out_modified_old,
                                               GPtrArray **out_modified_new,
                                               GCancellable *cancellable, GError **error);

/**
 * RpmOstreeDbDiffFlags:
 * @RPM_OSTREE_DB_DIFF_EXT_NONE: No flags.
 * @RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT: Don't error out if there is insufficient information
 *    to retrieve the list of packages (e.g. /usr/share/rpm or commit metadata missing).
 *
 * Since: 2017.12
 */
typedef enum
{
  RPM_OSTREE_DB_DIFF_EXT_NONE = 0,
  RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT = (1 << 0),
} RpmOstreeDbDiffExtFlags;

_RPMOSTREE_EXTERN gboolean rpm_ostree_db_diff_ext (
    OstreeRepo *repo, const char *orig_ref, const char *new_ref, RpmOstreeDbDiffExtFlags flags,
    GPtrArray **out_removed, GPtrArray **out_added, GPtrArray **out_modified_old,
    GPtrArray **out_modified_new, GCancellable *cancellable, GError **error);
G_END_DECLS
