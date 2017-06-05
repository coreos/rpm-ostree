/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

typedef struct RpmOstreeOrigin RpmOstreeOrigin;
RpmOstreeOrigin *rpmostree_origin_ref (RpmOstreeOrigin *origin);
void rpmostree_origin_unref (RpmOstreeOrigin *origin);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeOrigin, rpmostree_origin_unref)

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile *keyfile,
                                GError **error);

static inline
RpmOstreeOrigin *
rpmostree_origin_parse_deployment (OstreeDeployment *deployment,
                                   GError **error)
{
  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
      return NULL;
    }
  return rpmostree_origin_parse_keyfile (origin, error);
}

RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin);

gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin);

const char *const*
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin);

gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin);

void
rpmostree_origin_get_live_state (RpmOstreeOrigin *origin,
                                 char           **out_inprogress,
                                 char           **out_live);

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value);

GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin);

void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin,
                                           gboolean regenerate,
                                           char **args);

void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin,
                                      const char      *checksum,
                                      const char      *version);

gboolean
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin,
                             const char      *new_refspec,
                             GError         **error);

gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin   *origin,
                               char             **packages,
                               gboolean           local,
                               GError           **error);

gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin  *origin,
                                  char            **packages,
                                  GError          **error);

void
rpmostree_origin_set_live_state (RpmOstreeOrigin *origin,
                                 const char      *inprogress,
                                 const char      *live);
