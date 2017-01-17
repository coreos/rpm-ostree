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

#include "config.h"

#include "string.h"

#include "rpmostree-origin.h"

struct RpmOstreeOrigin {
  guint refcount;
  GKeyFile *kf;
  char *refspec;
  char **packages;
  char *override_commit;
};

static GKeyFile *
keyfile_dup (GKeyFile *kf)
{
  GKeyFile *ret = g_key_file_new ();
  gsize length = 0;
  g_autofree char *data = g_key_file_to_data (kf, &length, NULL);

  g_key_file_load_from_data (ret, data, length, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return ret;
}

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile         *origin,
                                RpmOstreeOriginFlags flags,
                                GError          **error)
{
  g_autoptr(RpmOstreeOrigin) ret = NULL;

  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->kf = keyfile_dup (origin);

  /* NOTE hack here - see https://github.com/ostreedev/ostree/pull/343 */
  g_key_file_remove_key (ret->kf, "origin", "unlocked", NULL);

  if ((flags & RPMOSTREE_ORIGIN_FLAGS_IGNORE_UNCONFIGURED) == 0)
    {
      g_autofree char *unconfigured_state = NULL;

      /* If explicit action by the OS creator is requried to upgrade, print their text as an error */
      unconfigured_state = g_key_file_get_string (origin, "origin", "unconfigured-state", NULL);
      if (unconfigured_state)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "origin unconfigured-state: %s", unconfigured_state);
          return NULL;
        }
    }

  ret->refspec = g_key_file_get_string (ret->kf, "origin", "refspec", NULL);
  if (!ret->refspec)
    {
      ret->refspec = g_key_file_get_string (ret->kf, "origin", "baserefspec", NULL);
      if (!ret->refspec)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No origin/refspec or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree");
          return NULL;
        }

      ret->packages = g_key_file_get_string_list (ret->kf, "packages", "requested", NULL, NULL);
    }

  /* Canonicalize NULL to an empty list for sanity */
  if (!ret->packages)
    ret->packages = g_new0 (char*, 1);

  ret->override_commit = g_key_file_get_string (ret->kf, "origin", "override-commit", NULL);

  return g_steal_pointer (&ret);
}

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return origin->refspec;
}

const char *const*
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return (const char * const*)origin->packages;
}

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return origin->override_commit;
}

gboolean
rpmostree_origin_is_locally_assembled (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  return g_strv_length (origin->packages) > 0;
}

GKeyFile *
rpmostree_origin_get_keyfile (RpmOstreeOrigin *origin)
{
  return origin->kf;
}

GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  return keyfile_dup (origin->kf);
}

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value)
{
  return g_key_file_get_string (origin->kf, section, value, NULL);
}

RpmOstreeOrigin*
rpmostree_origin_ref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  origin->refcount++;
  return origin;
}

void
rpmostree_origin_unref (RpmOstreeOrigin *origin)
{
  g_assert_cmpint (origin->refcount, >, 0);
  origin->refcount--;
  if (origin->refcount > 0)
    return;
  g_key_file_unref (origin->kf);
  g_free (origin->refspec);
  g_strfreev (origin->packages);
  g_free (origin);
}
