/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <ostree.h>

#include "rpmostreed-types.h"

G_BEGIN_DECLS

gchar *    rpmostreed_generate_object_path (const gchar  *base,
                                            const gchar  *part,
                                            ...);

gchar *    rpmostreed_generate_object_path_from_va (const gchar *base,
                                                    const gchar  *part,
                                                    va_list va);

gboolean   rpmostreed_refspec_parse_partial (const gchar *new_provided_refspec,
                                             const gchar *base_refspec,
                                             gchar **out_refspec,
                                             GError **error);

/* XXX These pull-ancestry and lookup-version functions should eventually
 *     be integrated into libostree, but it's still a bit premature to do
 *     so now.  Version integration in ostree needs more design work. */

typedef gboolean (*RpmostreedCommitVisitor) (OstreeRepo  *repo,
                                             const char  *checksum,
                                             GVariant    *commit,
                                             gpointer     user_data,
                                             gboolean    *out_stop,
                                             GError     **error);

gboolean   rpmostreed_repo_pull_ancestry (OstreeRepo               *repo,
                                          const char               *refspec,
                                          RpmostreedCommitVisitor   visitor,
                                          gpointer                  visitor_data,
                                          OstreeAsyncProgress      *progress,
                                          GCancellable             *cancellable,
                                          GError                  **error);

gboolean   rpmostreed_repo_lookup_version (OstreeRepo           *repo,
                                           const char           *refspec,
                                           const char           *version,
                                           OstreeAsyncProgress  *progress,
                                           GCancellable         *cancellable,
                                           char                **out_checksum,
                                           GError              **error);

gboolean rpmostreed_repo_lookup_checksum (OstreeRepo           *repo,
                                          const char           *refspec,
                                          const char           *checksum,
                                          OstreeAsyncProgress  *progress,
                                          GCancellable         *cancellable,
                                          GError              **error);

gboolean   rpmostreed_repo_lookup_cached_version (OstreeRepo    *repo,
                                                  const char    *refspec,
                                                  const char    *version,
                                                  GCancellable  *cancellable,
                                                  char         **out_checksum,
                                                  GError       **error);

gboolean   rpmostreed_parse_revision (const char  *revision,
                                      char       **out_checksum,
                                      char       **out_version,
                                      GError     **error);

gboolean   check_sd_inhibitor_locks (GCancellable    *cancellable,
                                     GError         **error);

G_END_DECLS