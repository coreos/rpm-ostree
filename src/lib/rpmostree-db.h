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

G_BEGIN_DECLS

typedef struct RpmOstreeDbQueryResult RpmOstreeDbQueryResult;

_RPMOSTREE_EXTERN GType rpm_ostree_db_query_result_get_type (void);
_RPMOSTREE_EXTERN const char *const *rpm_ostree_db_query_result_get_packages (RpmOstreeDbQueryResult *queryresult);

_RPMOSTREE_EXTERN RpmOstreeDbQueryResult *rpm_ostree_db_query_ref (RpmOstreeDbQueryResult *result);
_RPMOSTREE_EXTERN void rpm_ostree_db_query_unref (RpmOstreeDbQueryResult *result);

_RPMOSTREE_EXTERN RpmOstreeDbQueryResult *rpm_ostree_db_query (OstreeRepo               *repo,
                                                               const char               *ref,
                                                               GVariant                 *query,
                                                               GCancellable             *cancellable,
                                                               GError                  **error);

G_END_DECLS
