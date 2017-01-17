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

typedef enum {
  RPMOSTREE_ORIGIN_FLAGS_IGNORE_UNCONFIGURED = (1 << 0)
} RpmOstreeOriginFlags;

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile *keyfile,
                                RpmOstreeOriginFlags flags,
                                GError **error);

static inline
RpmOstreeOrigin *
rpmostree_origin_parse_deployment_ex (OstreeDeployment *deployment,
                                      RpmOstreeOriginFlags flags,
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
  return rpmostree_origin_parse_keyfile (origin, flags, error);
}

static inline
RpmOstreeOrigin *
rpmostree_origin_parse_deployment (OstreeDeployment *deployment,
                                   GError **error)
{
  return rpmostree_origin_parse_deployment_ex (deployment, 0, error);
}

gboolean
rpmostree_origin_is_locally_assembled (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin);

const char *const*
rpmostree_origin_get_packages (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin);

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value);

GKeyFile *
rpmostree_origin_get_keyfile (RpmOstreeOrigin *origin);
GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin);

