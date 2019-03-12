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
#include <rpmostree.h>

#define RPMOSTREE_DB_DIFF_VARIANT_FORMAT G_VARIANT_TYPE ("a(sua{sv})")

typedef enum {
  RPM_OSTREE_PACKAGE_ADDED,
  RPM_OSTREE_PACKAGE_REMOVED,
  RPM_OSTREE_PACKAGE_UPGRADED,
  RPM_OSTREE_PACKAGE_DOWNGRADED
} RpmOstreePackageDiffTypes;

gboolean
rpm_ostree_db_diff_variant (OstreeRepo *repo,
                            const char *from_rev,
                            const char *to_rev,
                            gboolean    allow_noent,
                            GVariant  **out_variant,
                            GCancellable *cancellable,
                            GError **error);
