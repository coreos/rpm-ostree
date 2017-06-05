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

#include "libglnx.h"
#include "rpmostree-origin.h"
#include "rpmostree-util.h"

struct RpmOstreeOrigin {
  guint refcount;

  /* this is the single source of truth */
  GKeyFile *kf;

  char *cached_refspec;
  char *cached_override_commit;
  char *cached_unconfigured_state;
  char **cached_initramfs_args;
  GHashTable *cached_packages;
  GHashTable *cached_local_packages;
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
                                GError          **error)
{
  g_autoptr(RpmOstreeOrigin) ret = NULL;

  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->kf = keyfile_dup (origin);

  ret->cached_packages = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);
  ret->cached_local_packages = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_free);

  /* NOTE hack here - see https://github.com/ostreedev/ostree/pull/343 */
  g_key_file_remove_key (ret->kf, "origin", "unlocked", NULL);

  ret->cached_unconfigured_state = g_key_file_get_string (ret->kf, "origin", "unconfigured-state", NULL);

  ret->cached_refspec = g_key_file_get_string (ret->kf, "origin", "refspec", NULL);
  if (!ret->cached_refspec)
    {
      g_auto(GStrv) packages = NULL;

      ret->cached_refspec = g_key_file_get_string (ret->kf, "origin", "baserefspec", NULL);
      if (!ret->cached_refspec)
        return glnx_null_throw (error, "No origin/refspec or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree");

      packages = g_key_file_get_string_list (ret->kf, "packages", "requested", NULL, NULL);
      for (char **it = packages; it && *it; it++)
        g_hash_table_add (ret->cached_packages, g_steal_pointer (it));

      g_strfreev (packages);
      packages = g_key_file_get_string_list (ret->kf, "packages", "requested-local", NULL, NULL);
      for (char **it = packages; it && *it; it++)
        {
          const char *nevra = *it;
          g_autofree char *sha256 = NULL;

          if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
            return glnx_null_throw (error, "Invalid SHA-256 NEVRA string: %s", nevra);

          g_hash_table_replace (ret->cached_local_packages,
                                g_strdup (nevra), g_steal_pointer (&sha256));
        }
    }

  ret->cached_override_commit =
    g_key_file_get_string (ret->kf, "origin", "override-commit", NULL);

  ret->cached_initramfs_args =
    g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-args", NULL, NULL);

  return g_steal_pointer (&ret);
}

RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin)
{
  return rpmostree_origin_parse_keyfile (origin->kf, NULL);
}

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return origin->cached_refspec;
}

GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_packages;
}

GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_local_packages;
}

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return origin->cached_override_commit;
}

gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "regenerate-initramfs", NULL);
}

const char *const*
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (const char * const*)origin->cached_initramfs_args;
}

const char*
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return origin->cached_unconfigured_state;
}

/* Determines whether the origin hints at local assembly being required. In some
 * cases, no assembly might actually be required (e.g. if requested packages are
 * already in the base). */
gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return rpmostree_origin_get_regenerate_initramfs (origin) ||
        (g_hash_table_size (origin->cached_packages) > 0) ||
        (g_hash_table_size (origin->cached_local_packages) > 0);
}

void
rpmostree_origin_get_live_state (RpmOstreeOrigin *origin,
                                 char           **out_inprogress,
                                 char           **out_live)
{
  if (out_inprogress)
    *out_inprogress = g_key_file_get_string (origin->kf, "rpmostree-ex-live", "inprogress", NULL);
  if (out_live)
    *out_live = g_key_file_get_string (origin->kf, "rpmostree-ex-live", "commit", NULL);
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
  g_assert (origin);
  g_assert_cmpint (origin->refcount, >, 0);
  origin->refcount--;
  if (origin->refcount > 0)
    return;
  g_key_file_unref (origin->kf);
  g_free (origin->cached_refspec);
  g_free (origin->cached_unconfigured_state);
  g_strfreev (origin->cached_initramfs_args);
  g_clear_pointer (&origin->cached_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_packages, g_hash_table_unref);
  g_free (origin);
}

void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin,
                                           gboolean regenerate,
                                           char **args)
{
  const char *section = "rpmostree";
  const char *regeneratek = "regenerate-initramfs";
  const char *argsk = "initramfs-args";

  g_strfreev (origin->cached_initramfs_args);

  if (regenerate)
    {
      g_key_file_set_boolean (origin->kf, section, regeneratek, TRUE);
      if (args && *args)
        {
          g_key_file_set_string_list (origin->kf, section, argsk,
                                      (const char *const*)args,
                                      g_strv_length (args));
        }
      else
        g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }
  else
    {
      g_key_file_remove_key (origin->kf, section, regeneratek, NULL);
      g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }

  origin->cached_initramfs_args =
    g_key_file_get_string_list (origin->kf, "rpmostree", "initramfs-args",
                                NULL, NULL);
}

void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin,
                                      const char      *checksum,
                                      const char      *version)
{
  if (checksum != NULL)
    {
      g_key_file_set_string (origin->kf, "origin", "override-commit", checksum);

      /* Add a comment with the version, to be nice. */
      if (version != NULL)
        {
          g_autofree char *comment =
            g_strdup_printf ("Version %s [%.10s]", version, checksum);
          g_key_file_set_comment (origin->kf, "origin", "override-commit", comment, NULL);
        }
    }
  else
    {
      g_key_file_remove_key (origin->kf, "origin", "override-commit", NULL);
    }

  g_free (origin->cached_override_commit);
  origin->cached_override_commit = g_strdup (checksum);
}

/* updates an origin's refspec without migrating format */
static gboolean
origin_set_refspec (GKeyFile   *origin,
                    const char *new_refspec,
                    GError    **error)
{
  if (g_key_file_has_key (origin, "origin", "baserefspec", error))
    {
      g_key_file_set_value (origin, "origin", "baserefspec", new_refspec);
      return TRUE;
    }

  if (error && *error)
    return FALSE;

  g_key_file_set_value (origin, "origin", "refspec", new_refspec);
  return TRUE;
}

gboolean
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin,
                             const char      *new_refspec,
                             GError         **error)
{
  if (!origin_set_refspec (origin->kf, new_refspec, error))
    return FALSE;

  /* we don't want to carry any commit overrides during a rebase */
  rpmostree_origin_set_override_commit (origin, NULL, NULL);

  g_free (origin->cached_refspec);
  origin->cached_refspec = g_strdup (new_refspec);

  return TRUE;
}

/* Like g_key_file_set_string(), but remove the key if @value is NULL */
static void
set_or_unset_str (GKeyFile *kf,
                  const char *group,
                  const char *key,
                  const char *value)
{
  if (!value)
    (void) g_key_file_remove_key (kf, group, key, NULL);
  else
    (void) g_key_file_set_string (kf, group, key, value);
}

void
rpmostree_origin_set_live_state (RpmOstreeOrigin *origin,
                                 const char      *inprogress,
                                 const char      *live)
{
  if (!inprogress && !live)
    (void) g_key_file_remove_group (origin->kf, "rpmostree-ex-live", NULL);
  else
    {
      set_or_unset_str (origin->kf, "rpmostree-ex-live", "inprogress", inprogress);
      set_or_unset_str (origin->kf, "rpmostree-ex-live", "commit", live);
    }
}

static void
update_keyfile_pkgs_from_cache (RpmOstreeOrigin *origin,
                                gboolean         local)
{
  GHashTable *pkgs = local ? origin->cached_local_packages
                           : origin->cached_packages;

  /* we're abusing a bit the concept of cache here, though
   * it's just easier to go from cache to origin */

  if (local)
    {
      GHashTableIter it;
      gpointer k, v;

      g_autoptr(GPtrArray) sha256_nevra =
        g_ptr_array_new_with_free_func (g_free);

      g_hash_table_iter_init (&it, origin->cached_local_packages);
      while (g_hash_table_iter_next (&it, &k, &v))
        g_ptr_array_add (sha256_nevra, g_strconcat (v, ":", k, NULL));

      g_key_file_set_string_list (origin->kf, "packages", "requested-local",
                                  (const char *const*)sha256_nevra->pdata,
                                  sha256_nevra->len);
    }
  else
    {
      g_autofree char **pkgv =
        (char**)g_hash_table_get_keys_as_array (pkgs, NULL);
      g_key_file_set_string_list (origin->kf, "packages", "requested",
                                  (const char *const*)pkgv,
                                  g_strv_length (pkgv));
    }

  if (g_hash_table_size (pkgs) > 0)
    {
      /* migrate to baserefspec model */
      g_key_file_set_value (origin->kf, "origin", "baserefspec",
                            origin->cached_refspec);
      g_key_file_remove_key (origin->kf, "origin", "refspec", NULL);
    }
}

gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin   *origin,
                               char             **packages,
                               gboolean           local,
                               GError           **error)
{
  gboolean changed = FALSE;

  for (char **it = packages; it && *it; it++)
    {
      gboolean requested, requested_local;
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (local)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      requested = g_hash_table_contains (origin->cached_packages, pkg);
      requested_local = g_hash_table_contains (origin->cached_local_packages, pkg);

      /* The list of packages is really a list of provides, so string equality
       * is a bit weird here. Multiple provides can resolve to the same package
       * and we allow that. But still, let's make sure that silly users don't
       * request the exact string. */

      /* Also note that we check in *both* the requested and the requested-local
       * list: requested-local pkgs are treated like requested pkgs in the core.
       * The only "magical" thing about them is that requested-local pkgs are
       * specifically looked for in the pkgcache. Additionally, making sure the
       * strings are unique allow `rpm-ostree uninstall` to know exactly what
       * the user means. */

      if (requested || requested_local)
        {
          if (requested)
            return glnx_throw (error, "Package/capability '%s' is already requested", pkg);
          else
            return glnx_throw (error, "Package '%s' is already layered", pkg);
        }

      if (local)
        g_hash_table_insert (origin->cached_local_packages,
                             g_strdup (pkg), g_steal_pointer (&sha256));
      else
        g_hash_table_add (origin->cached_packages, g_strdup (pkg));
      changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, local);

  return TRUE;
}

gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin  *origin,
                                  char            **packages,
                                  GError          **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;

  for (char **it = packages; it && *it; it++)
    {
      if (g_hash_table_contains (origin->cached_local_packages, *it))
        {
          g_hash_table_remove (origin->cached_local_packages, *it);
          local_changed = TRUE;
        }
      else if (g_hash_table_contains (origin->cached_packages, *it))
        {
          g_hash_table_remove (origin->cached_packages, *it);
          changed = TRUE;
        }
      else
        {
          return glnx_throw (error, "Package/capability '%s' is not currently requested", *it);
        }
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, TRUE);

  return TRUE;
}
