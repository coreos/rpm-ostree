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
#include <systemd/sd-journal.h>

#include "libglnx.h"
#include "rpmostree-core.h"
#include "rpmostree-origin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"

struct RpmOstreeOrigin
{
  guint refcount;

  /* this is the single source of truth */
  std::optional<rust::Box<rpmostreecxx::Treefile> > treefile;

  /* this is used for convenience while we migrate; we always sync back to the treefile */
  GKeyFile *kf;

  /* Branch name or pinned to commit*/
  rpmostreecxx::RefspecType refspec_type;
  char *cached_refspec;

  /* Container image digest, if tracking a container image reference */
  char *cached_digest;

  /* Version data that goes along with the refspec */
  char *cached_override_commit;

  char *cached_unconfigured_state;
  char **cached_initramfs_args;
  GHashTable *cached_initramfs_etc_files;         /* set of paths */
  GHashTable *cached_packages;                    /* set of reldeps */
  GHashTable *cached_modules_enable;              /* set of module specs to enable */
  GHashTable *cached_modules_install;             /* set of module specs to install */
  GHashTable *cached_local_packages;              /* NEVRA --> header sha256 */
  GHashTable *cached_local_fileoverride_packages; /* NEVRA --> header sha256 */
  /* GHashTable *cached_overrides_replace;         XXX: NOT IMPLEMENTED YET */
  GHashTable *cached_overrides_local_replace; /* NEVRA --> header sha256 */
  GHashTable *cached_overrides_remove;        /* set of pkgnames (no EVRA) */
};

RpmOstreeOrigin *
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
  g_clear_pointer (&origin->cached_modules_enable, g_hash_table_unref);
  g_clear_pointer (&origin->cached_modules_install, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_fileoverride_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_local_replace, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_remove, g_hash_table_unref);
  g_clear_pointer (&origin->cached_initramfs_etc_files, g_hash_table_unref);
  g_free (origin);
}

static gboolean
sync_treefile (RpmOstreeOrigin *self)
{
  self->treefile.reset ();
  g_autoptr (GError) local_error = NULL;
  self->treefile = ROSCXX_VAL (origin_to_treefile (*self->kf), &local_error);
  g_assert_no_error (local_error);
  return TRUE;
}

static GKeyFile *
keyfile_dup (GKeyFile *kf)
{
  GKeyFile *ret = g_key_file_new ();
  gsize length = 0;
  g_autofree char *data = g_key_file_to_data (kf, &length, NULL);

  g_key_file_load_from_data (ret, data, length, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return ret;
}

/* take <nevra:sha256> entries from keyfile and inserts them into hash table */
static gboolean
parse_packages_strv (GKeyFile *kf, const char *group, const char *key, gboolean has_sha256,
                     GHashTable *ht, GError **error)
{
  g_auto (GStrv) packages = g_key_file_get_string_list (kf, group, key, NULL, NULL);

  for (char **it = packages; it && *it; it++)
    {
      if (has_sha256)
        {
          const char *nevra = *it;
          g_autofree char *sha256 = NULL;
          if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", nevra);
          g_hash_table_replace (ht, g_strdup (nevra), util::move_nullify (sha256));
        }
      else
        {
          g_hash_table_add (ht, util::move_nullify (*it));
        }
    }

  return TRUE;
}

RpmOstreeOrigin *
rpmostree_origin_parse_deployment (OstreeDeployment *deployment, GError **error)
{
  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
      return NULL;
    }
  return rpmostree_origin_parse_keyfile (origin, error);
}

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile *origin, GError **error)
{
  g_autoptr (RpmOstreeOrigin) ret = NULL;

  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->kf = keyfile_dup (origin);
  ret->treefile = ROSCXX_VAL (origin_to_treefile (*ret->kf), error);

  ret->cached_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_enable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_install = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_local_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_local_fileoverride_packages
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_local_replace
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_remove = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_initramfs_etc_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  ret->cached_unconfigured_state
      = g_key_file_get_string (ret->kf, "origin", "unconfigured-state", NULL);

  /* Note that the refspec type can be inferred from the key in the origin file, where
   * the `RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY` and `RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY` keys
   * may correspond to either TYPE_OSTREE or TYPE_CHECKSUM (further classification is required), and
   * `RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY` always only corresponds to TYPE_CONTAINER. */
  g_autofree char *ost_refspec
      = g_key_file_get_string (ret->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY, NULL);
  g_autofree char *imgref
      = g_key_file_get_string (ret->kf, "origin", RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY, NULL);
  if (!ost_refspec)
    {
      /* See if ostree refspec is baserefspec. */
      ost_refspec = g_key_file_get_string (ret->kf, "origin",
                                           RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY, NULL);
      if (!ost_refspec && !imgref)
        return (RpmOstreeOrigin *)glnx_null_throw (
            error,
            "No origin/%s, origin/%s, or origin/%s "
            "in current deployment origin; cannot handle via rpm-ostree",
            RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY, RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY,
            RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY);
    }
  if (ost_refspec && imgref)
    {
      return (RpmOstreeOrigin *)glnx_null_throw (
          error, "Refspec expressed by multiple keys in deployment origin");
    }
  else if (ost_refspec)
    {
      /* Classify to distinguish between TYPE_CHECKSUM and TYPE_OSTREE */
      ret->refspec_type = rpmostreecxx::refspec_classify (ost_refspec);
      ret->cached_refspec = util::move_nullify (ost_refspec);
      ret->cached_override_commit
          = g_key_file_get_string (ret->kf, "origin", "override-commit", NULL);
    }
  else if (imgref)
    {
      ret->refspec_type = rpmostreecxx::RefspecType::Container;
      ret->cached_refspec = util::move_nullify (imgref);

      ret->cached_digest
          = g_key_file_get_string (ret->kf, "origin", "container-image-reference-digest", NULL);
    }
  else
    {
      g_assert_not_reached ();
    }

  if (!parse_packages_strv (ret->kf, "packages", "requested", FALSE, ret->cached_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "packages", "requested-local", TRUE,
                            ret->cached_local_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "packages", "requested-local-fileoverride", TRUE,
                            ret->cached_local_fileoverride_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "modules", "enable", FALSE, ret->cached_modules_enable, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "modules", "install", FALSE, ret->cached_modules_install,
                            error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "remove", FALSE, ret->cached_overrides_remove,
                            error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "replace-local", TRUE,
                            ret->cached_overrides_local_replace, error))
    return FALSE;

  g_auto (GStrv) initramfs_etc_files
      = g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-etc", NULL, NULL);
  for (char **f = initramfs_etc_files; f && *f; f++)
    g_hash_table_add (ret->cached_initramfs_etc_files, util::move_nullify (*f));

  ret->cached_initramfs_args
      = g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-args", NULL, NULL);

  // We will eventually start converting origin to treefile, this helps us
  // debug cases that may fail currently.
  rpmostreecxx::origin_validate_roundtrip (*ret->kf);

  return util::move_nullify (ret);
}

/* Mutability: getter */
RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin)
{
  g_autoptr (GError) local_error = NULL;
  RpmOstreeOrigin *ret = rpmostree_origin_parse_keyfile (origin->kf, &local_error);
  g_assert_no_error (local_error);
  return ret;
}

/* This is useful if the origin is meant to be used to generate a *new* deployment, as
 * opposed to simply gathering information about an existing one. In such cases, there are
 * some things that we do not generally want to apply to a new deployment. */
/* Mutability: setter */
void
rpmostree_origin_remove_transient_state (RpmOstreeOrigin *origin)
{
  /* first libostree-known things */
  ostree_deployment_origin_remove_transient_state (origin->kf);

  /* this is already covered by the above, but the below also updates the cached value */
  rpmostree_origin_set_override_commit (origin, NULL);

  sync_treefile (origin);
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  // This is awkward: the treefile supports no base refspec being set since none is in the compose
  // case. But in the derive case, there should *always* be a base refspec, so verify this. Ideally
  // we'd have a different type for derivation treefiles. It's not the right time to fix this yet.
  auto r = (*origin->treefile)->get_base_refspec ();
  g_assert_cmpstr (r.c_str (), !=, "");
  return r;
}

rust::String
rpmostree_origin_get_custom_url (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_origin_custom_url ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_origin_custom_description ();
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_packages;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_modules_enable (RpmOstreeOrigin *origin)
{
  return origin->cached_modules_enable;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_local_packages;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_local_fileoverride_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_local_fileoverride_packages;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_remove;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_local_replace;
}

/* Mutability: getter */
const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return origin->cached_override_commit;
}

/* Mutability: getter */
GHashTable *
rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return origin->cached_initramfs_etc_files;
}

/* Mutability: getter */
gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "regenerate-initramfs", NULL);
}

/* Mutability: getter */
const char *const *
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (const char *const *)origin->cached_initramfs_args;
}

/* Mutability: getter */
const char *
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return origin->cached_unconfigured_state;
}

/* Determines whether the origin hints at local assembly being required. In some
 * cases, no assembly might actually be required (e.g. if requested packages are
 * already in the base). IOW:
 *    FALSE --> definitely does not require local assembly
 *    TRUE  --> maybe requires assembly, need to investigate further by doing work
 */
/* Mutability: getter */
gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return rpmostree_origin_get_cliwrap (origin) || rpmostree_origin_get_regenerate_initramfs (origin)
         || (g_hash_table_size (origin->cached_initramfs_etc_files) > 0)
         || rpmostree_origin_has_any_packages (origin) ||
         /* Technically, alone it doesn't require require assembly, but it still
          * requires fetching repo metadata to validate (remember: modules are a
          * pure rpmmd concept). This means we may pay the cost of an unneeded
          * tree checkout, but it's not worth trying to optimize for it. */
         (g_hash_table_size (origin->cached_modules_enable) > 0);
}

/* Returns TRUE if this origin contains overlay or override packages */
/* Mutability: getter */
gboolean
rpmostree_origin_has_any_packages (RpmOstreeOrigin *origin)
{
  return (g_hash_table_size (origin->cached_packages) > 0)
         || (g_hash_table_size (origin->cached_local_packages) > 0)
         || (g_hash_table_size (origin->cached_local_fileoverride_packages) > 0)
         || (g_hash_table_size (origin->cached_overrides_local_replace) > 0)
         || (g_hash_table_size (origin->cached_overrides_remove) > 0)
         || (g_hash_table_size (origin->cached_modules_install) > 0);
}

/* Mutability: getter */
GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  return keyfile_dup (origin->kf);
}

static void
update_string_list_from_hash_table (RpmOstreeOrigin *origin, const char *group, const char *key,
                                    GHashTable *values)
{
  g_autofree char **strv = (char **)g_hash_table_get_keys_as_array (values, NULL);
  g_key_file_set_string_list (origin->kf, group, key, (const char *const *)strv,
                              g_strv_length (strv));
}

/* Mutability: setter */
void
rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin, char **paths,
                                            gboolean *out_changed)
{
  gboolean changed = FALSE;
  for (char **path = paths; path && *path; path++)
    changed = (g_hash_table_add (origin->cached_initramfs_etc_files, g_strdup (*path)) || changed);

  if (changed)
    update_string_list_from_hash_table (origin, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin, char **paths,
                                              gboolean *out_changed)
{
  gboolean changed = FALSE;
  for (char **path = paths; path && *path; path++)
    changed = (g_hash_table_remove (origin->cached_initramfs_etc_files, *path) || changed);

  if (changed)
    update_string_list_from_hash_table (origin, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin, gboolean *out_changed)
{
  const gboolean changed = (g_hash_table_size (origin->cached_initramfs_etc_files) > 0);
  g_hash_table_remove_all (origin->cached_initramfs_etc_files);
  if (changed)
    update_string_list_from_hash_table (origin, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin, gboolean regenerate,
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
          g_key_file_set_string_list (origin->kf, section, argsk, (const char *const *)args,
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

  origin->cached_initramfs_args
      = g_key_file_get_string_list (origin->kf, "rpmostree", "initramfs-args", NULL, NULL);

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin, const char *checksum)
{
  if (checksum != NULL)
    {
      g_key_file_set_string (origin->kf, "origin", "override-commit", checksum);
    }
  else
    {
      g_key_file_remove_key (origin->kf, "origin", "override-commit", NULL);
    }

  g_free (origin->cached_override_commit);
  origin->cached_override_commit = g_strdup (checksum);

  sync_treefile (origin);
}

/* Mutability: getter */
gboolean
rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "ex-cliwrap", NULL);
}

/* Mutability: setter */
void
rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, gboolean cliwrap)
{
  const char k[] = "rpmostree";
  const char v[] = "ex-cliwrap";
  if (cliwrap)
    g_key_file_set_boolean (origin->kf, k, v, TRUE);
  else
    g_key_file_remove_key (origin->kf, k, v, NULL);

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin, const char *new_refspec,
                                    const char *custom_origin_url,
                                    const char *custom_origin_description)
{
  /* Require non-empty strings */
  if (custom_origin_url)
    {
      g_assert (*custom_origin_url);
      g_assert (custom_origin_description && *custom_origin_description);
    }

  /* We don't want to carry any commit overrides or version pinning during a
   * rebase by default.
   */
  rpmostree_origin_set_override_commit (origin, NULL);

  /* See related code in rpmostree_origin_parse_keyfile() */
  origin->refspec_type = rpmostreecxx::refspec_classify (new_refspec);
  g_free (origin->cached_refspec);
  origin->cached_refspec = g_strdup (new_refspec);
  /* Note the following sets different keys depending on the type of refspec;
   * TYPE_OSTREE and TYPE_CHECKSUM may set either `RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY`
   * or `RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY`, while TYPE_CONTAINER will set the
   * `RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY` key. */
  switch (origin->refspec_type)
    {
    case rpmostreecxx::RefspecType::Checksum:
    case rpmostreecxx::RefspecType::Ostree:
      {
        /* Remove `TYPE_CONTAINER`-related keys */
        g_key_file_remove_key (origin->kf, "origin", RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY, NULL);
        g_key_file_remove_key (origin->kf, "origin", "container-image-reference-digest", NULL);

        const char *refspec_key
            = g_key_file_has_key (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY,
                                  NULL)
                  ? RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY
                  : RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY;
        g_key_file_set_string (origin->kf, "origin", refspec_key, origin->cached_refspec);
        if (!custom_origin_url)
          {
            g_key_file_remove_key (origin->kf, "origin", "custom-url", NULL);
            g_key_file_remove_key (origin->kf, "origin", "custom-description", NULL);
          }
        else
          {
            /* Custom origins have to be checksums */
            g_assert (origin->refspec_type == rpmostreecxx::RefspecType::Checksum);
            g_key_file_set_string (origin->kf, "origin", "custom-url", custom_origin_url);
            if (custom_origin_description)
              g_key_file_set_string (origin->kf, "origin", "custom-description",
                                     custom_origin_description);
          }
      }
      break;
    case rpmostreecxx::RefspecType::Container:
      {
        /* Remove `TYPE_OSTREE` and `TYPE_CHECKSUM`-related keys */
        g_assert (!custom_origin_url);
        g_key_file_remove_key (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY, NULL);
        g_key_file_remove_key (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY,
                               NULL);
        g_key_file_remove_key (origin->kf, "origin", "custom-url", NULL);
        g_key_file_remove_key (origin->kf, "origin", "custom-description", NULL);

        g_key_file_set_string (origin->kf, "origin", RPMOSTREE_REFSPEC_CONTAINER_ORIGIN_KEY,
                               origin->cached_refspec);
      }
      break;
    }

  sync_treefile (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin, const char *new_refspec)
{
  // NB: calls sync_treefile
  return rpmostree_origin_set_rebase_custom (origin, new_refspec, NULL, NULL);
}

/* If necessary, switch to `baserefspec` when changing the origin
 * to something core ostree doesnt't understand, i.e.
 * when `ostree admin upgrade` would no longer do the right thing. */
static void
sync_baserefspec (RpmOstreeOrigin *origin)
{
  /* If we're based on a container image reference, this is not necessary since
   * `ostree admin upgrade` won't work regardless of whether or not packages are
   * layered. Though this may change in the future. */
  if (origin->refspec_type == rpmostreecxx::RefspecType::Container)
    return;
  if (rpmostree_origin_may_require_local_assembly (origin))
    {
      g_key_file_set_value (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY,
                            origin->cached_refspec);
      g_key_file_remove_key (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY, NULL);
    }
  else
    {
      g_key_file_set_value (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_ORIGIN_KEY,
                            origin->cached_refspec);
      g_key_file_remove_key (origin->kf, "origin", RPMOSTREE_REFSPEC_OSTREE_BASE_ORIGIN_KEY, NULL);
    }
}

static void
update_keyfile_pkgs_from_cache (RpmOstreeOrigin *origin, const char *group, const char *key,
                                GHashTable *pkgs, gboolean has_csum)
{
  /* we're abusing a bit the concept of cache here, though
   * it's just easier to go from cache to origin */

  if (g_hash_table_size (pkgs) == 0)
    {
      g_key_file_remove_key (origin->kf, group, key, NULL);
      sync_baserefspec (origin);
      return;
    }

  if (has_csum)
    {
      g_autoptr (GPtrArray) pkg_csum = g_ptr_array_new_with_free_func (g_free);
      GLNX_HASH_TABLE_FOREACH_KV (pkgs, const char *, k, const char *, v)
      g_ptr_array_add (pkg_csum, g_strconcat (v, ":", k, NULL));
      g_key_file_set_string_list (origin->kf, group, key, (const char *const *)pkg_csum->pdata,
                                  pkg_csum->len);
    }
  else
    {
      update_string_list_from_hash_table (origin, group, key, pkgs);
    }

  sync_baserefspec (origin);
}

static void
set_changed (gboolean *out, gboolean c)
{
  if (!out)
    return;
  *out = *out || c;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin *origin, char **packages, gboolean local,
                               gboolean fileoverride, gboolean allow_existing,
                               gboolean *out_changed, GError **error)
{
  if (!packages)
    return TRUE;
  gboolean changed = FALSE;

  for (char **it = packages; it && *it; it++)
    {
      gboolean requested, requested_local, requested_local_fileoverride;
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (local)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      requested = g_hash_table_contains (origin->cached_packages, pkg);
      requested_local = g_hash_table_contains (origin->cached_local_packages, pkg);
      requested_local_fileoverride
          = g_hash_table_contains (origin->cached_local_fileoverride_packages, pkg);

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

      if (requested || requested_local || requested_local_fileoverride)
        {
          if (allow_existing)
            continue;

          if (requested)
            return glnx_throw (error, "Package/capability '%s' is already requested", pkg);
          else
            return glnx_throw (error, "Package '%s' is already layered", pkg);
        }

      if (local)
        g_hash_table_insert (fileoverride ? origin->cached_local_fileoverride_packages
                                          : origin->cached_local_packages,
                             g_strdup (pkg), util::move_nullify (sha256));
      else
        g_hash_table_add (origin->cached_packages, g_strdup (pkg));
      changed = TRUE;
    }

  if (changed)
    {
      const char *key = "requested";
      GHashTable *ht = origin->cached_packages;
      if (local && !fileoverride)
        {
          key = "requested-local";
          ht = origin->cached_local_packages;
        }
      else if (local && fileoverride)
        {
          key = "requested-local-fileoverride";
          ht = origin->cached_local_fileoverride_packages;
        }
      update_keyfile_pkgs_from_cache (origin, "packages", key, ht, local);

      sync_treefile (origin);
    }

  set_changed (out_changed, changed);
  return TRUE;
}

static gboolean
build_name_to_nevra_map (GHashTable *nevras, GHashTable **out_name_to_nevra, GError **error)
{
  g_autoptr (GHashTable) name_to_nevra = /* nevra vals owned by @nevras */
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GLNX_HASH_TABLE_FOREACH (nevras, const char *, nevra)
  {
    g_autofree char *name = NULL;
    if (!rpmostree_decompose_nevra (nevra, &name, NULL, NULL, NULL, NULL, error))
      return FALSE;
    g_hash_table_insert (name_to_nevra, util::move_nullify (name), (gpointer)nevra);
  }

  *out_name_to_nevra = util::move_nullify (name_to_nevra);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin *origin, char **packages, gboolean allow_noent,
                                  gboolean *out_changed, GError **error)
{
  if (!packages)
    return TRUE;
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;
  gboolean local_fileoverride_changed = FALSE;

  /* lazily calculated */
  g_autoptr (GHashTable) name_to_nevra = NULL;
  g_autoptr (GHashTable) name_to_nevra_fileoverride = NULL;

  for (char **it = packages; it && *it; it++)
    {
      /* really, either a NEVRA (local RPM) or freeform provides request (from repo) */
      const char *package = *it;
      if (g_hash_table_remove (origin->cached_local_packages, package))
        local_changed = TRUE;
      else if (g_hash_table_remove (origin->cached_local_fileoverride_packages, package))
        local_fileoverride_changed = TRUE;
      else if (g_hash_table_remove (origin->cached_packages, package))
        changed = TRUE;
      else
        {
          if (!name_to_nevra)
            {
              if (!build_name_to_nevra_map (origin->cached_local_packages, &name_to_nevra, error))
                return FALSE;
              if (!build_name_to_nevra_map (origin->cached_local_fileoverride_packages,
                                            &name_to_nevra_fileoverride, error))
                return FALSE;
            }

          if (g_hash_table_contains (name_to_nevra, package))
            {
              g_assert (g_hash_table_remove (origin->cached_local_packages,
                                             g_hash_table_lookup (name_to_nevra, package)));
              local_changed = TRUE;
            }
          else if (g_hash_table_contains (name_to_nevra_fileoverride, package))
            {
              g_assert (
                  g_hash_table_remove (origin->cached_local_fileoverride_packages,
                                       g_hash_table_lookup (name_to_nevra_fileoverride, package)));
              local_fileoverride_changed = TRUE;
            }
          else if (!allow_noent)
            return glnx_throw (error, "Package/capability '%s' is not currently requested",
                               package);
        }
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested", origin->cached_packages,
                                    FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);
  if (local_fileoverride_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local-fileoverride",
                                    origin->cached_local_fileoverride_packages, TRUE);

  const gboolean any_changed = changed || local_changed || local_fileoverride_changed;

  set_changed (out_changed, any_changed);
  if (any_changed)
    sync_treefile (origin);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_modules (RpmOstreeOrigin *origin, char **modules, gboolean enable_only,
                              gboolean *out_changed, GError **error)
{
  if (!modules)
    return TRUE;
  const char *key = enable_only ? "enable" : "install";
  GHashTable *target = enable_only ? origin->cached_modules_enable : origin->cached_modules_install;
  gboolean changed = FALSE;
  for (char **mod = modules; mod && *mod; mod++)
    changed = (g_hash_table_add (target, g_strdup (*mod)) || changed);

  if (changed)
    {
      update_string_list_from_hash_table (origin, "modules", key, target);
      sync_treefile (origin);
    }

  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_modules (RpmOstreeOrigin *origin, char **modules, gboolean enable_only,
                                 gboolean *out_changed, GError **error)
{
  if (!modules)
    return TRUE;
  const char *key = enable_only ? "enable" : "install";
  GHashTable *target = enable_only ? origin->cached_modules_enable : origin->cached_modules_install;
  gboolean changed = FALSE;
  for (char **mod = modules; mod && *mod; mod++)
    changed = (g_hash_table_remove (target, *mod) || changed);

  if (changed)
    {
      update_string_list_from_hash_table (origin, "modules", key, target);
      sync_treefile (origin);
    }

  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_all_packages (RpmOstreeOrigin *origin, gboolean *out_changed,
                                      GError **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;
  gboolean local_fileoverride_changed = FALSE;
  gboolean modules_enable_changed = FALSE;
  gboolean modules_install_changed = FALSE;

  if (g_hash_table_size (origin->cached_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_packages);
      changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_local_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_local_packages);
      local_changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_local_fileoverride_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_local_fileoverride_packages);
      local_fileoverride_changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_modules_enable) > 0)
    {
      g_hash_table_remove_all (origin->cached_modules_enable);
      modules_enable_changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_modules_install) > 0)
    {
      g_hash_table_remove_all (origin->cached_modules_install);
      modules_install_changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested", origin->cached_packages,
                                    FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);
  if (local_fileoverride_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local-fileoverride",
                                    origin->cached_local_fileoverride_packages, TRUE);
  if (modules_enable_changed)
    update_keyfile_pkgs_from_cache (origin, "modules", "enable", origin->cached_modules_enable,
                                    FALSE);
  if (modules_install_changed)
    update_keyfile_pkgs_from_cache (origin, "modules", "install", origin->cached_modules_install,
                                    FALSE);

  changed = changed || local_changed || local_fileoverride_changed || modules_enable_changed
            || modules_install_changed;
  set_changed (out_changed, changed);
  if (changed)
    sync_treefile (origin);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_overrides (RpmOstreeOrigin *origin, char **packages,
                                RpmOstreeOriginOverrideType type, GError **error)
{
  gboolean changed = FALSE;
  for (char **it = packages; it && *it; it++)
    {
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      /* Check that the same overrides don't already exist. Of course, in the local replace
       * case, this doesn't catch same pkg name but different EVRA; we'll just barf at that
       * later on in the core. This is just an early easy sanity check. */
      if (g_hash_table_contains (origin->cached_overrides_remove, pkg)
          || g_hash_table_contains (origin->cached_overrides_local_replace, pkg))
        return glnx_throw (error, "Override already exists for package '%s'", pkg);

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        g_hash_table_insert (origin->cached_overrides_local_replace, g_strdup (pkg),
                             util::move_nullify (sha256));
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        g_hash_table_add (origin->cached_overrides_remove, g_strdup (pkg));
      else
        g_assert_not_reached ();

      changed = TRUE;
    }

  if (changed)
    {
      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                        origin->cached_overrides_local_replace, TRUE);
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                        origin->cached_overrides_remove, FALSE);
      else
        g_assert_not_reached ();

      sync_treefile (origin);
    }

  return TRUE;
}

/* Returns FALSE if the override does not exist. */
/* Mutability: setter */
gboolean
rpmostree_origin_remove_override (RpmOstreeOrigin *origin, const char *package,
                                  RpmOstreeOriginOverrideType type)
{
  if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
    {
      if (!g_hash_table_remove (origin->cached_overrides_local_replace, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                      origin->cached_overrides_local_replace, TRUE);
    }
  else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
    {
      if (!g_hash_table_remove (origin->cached_overrides_remove, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                      origin->cached_overrides_remove, FALSE);
    }
  else
    g_assert_not_reached ();

  sync_treefile (origin);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_all_overrides (RpmOstreeOrigin *origin, gboolean *out_changed,
                                       GError **error)
{
  gboolean remove_changed = (g_hash_table_size (origin->cached_overrides_remove) > 0);
  g_hash_table_remove_all (origin->cached_overrides_remove);

  gboolean local_replace_changed = (g_hash_table_size (origin->cached_overrides_local_replace) > 0);
  g_hash_table_remove_all (origin->cached_overrides_local_replace);

  if (remove_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "remove", origin->cached_overrides_remove,
                                    FALSE);
  if (local_replace_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                    origin->cached_overrides_local_replace, TRUE);

  const gboolean any_changed = remove_changed || local_replace_changed;
  set_changed (out_changed, any_changed);
  if (any_changed)
    sync_treefile (origin);
  return TRUE;
}
