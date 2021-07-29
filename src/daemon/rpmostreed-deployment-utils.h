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
#include <libdnf/libdnf.h>

#include "rpmostreed-types.h"

G_BEGIN_DECLS

char *          rpmostreed_deployment_generate_id (OstreeDeployment *deployment);

gboolean
rpmostreed_deployment_get_for_id (OstreeSysroot     *sysroot,
                                  const gchar       *deploy_id,
                                  OstreeDeployment **out_deployment,
                                  GError           **error);

OstreeDeployment *
                rpmostreed_deployment_get_for_index (OstreeSysroot *sysroot,
                                                     const gchar   *index,
                                                     GError       **error);

GVariant *      rpmostreed_deployment_generate_blank_variant (void);

gboolean        rpmostreed_deployment_generate_variant (OstreeSysroot    *sysroot,
                                                        OstreeDeployment *deployment,
                                                        const char       *booted_id,
                                                        OstreeRepo       *repo,
                                                        gboolean          filter,
                                                        GVariant        **out_variant,
                                                        GError          **error);

GVariant *      rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment,
                                                                   OstreeRepo       *repo,
                                                                   const char       *refspec,
                                                                   const char       *checksum,
                                                                   GError          **error);

gboolean        rpmostreed_update_generate_variant (OstreeDeployment  *booted_deployment,
                                                    OstreeDeployment  *staged_deployment,
                                                    OstreeRepo        *repo,
                                                    DnfSack           *sack,
                                                    GVariant         **out_update,
                                                    GCancellable      *cancellable,
                                                    GError           **error);

G_END_DECLS
