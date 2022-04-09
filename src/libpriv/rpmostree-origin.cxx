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

  char *cached_unconfigured_state;
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
  g_free (origin->cached_unconfigured_state);
  g_clear_pointer (&origin->cached_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_modules_enable, g_hash_table_unref);
  g_clear_pointer (&origin->cached_modules_install, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_fileoverride_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_local_replace, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_remove, g_hash_table_unref);
  g_free (origin);
}

static void
sync_treefile (RpmOstreeOrigin *self)
{
  self->treefile.reset ();
  // Note this may throw a C++ exception
  self->treefile = rpmostreecxx::origin_to_treefile (*self->kf);
}

static void
sync_origin (RpmOstreeOrigin *self)
{
  g_autoptr (GKeyFile) kf = rpmostreecxx::treefile_to_origin (**self->treefile);
  g_clear_pointer (&self->kf, g_key_file_unref);
  self->kf = g_key_file_ref (kf);
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
  ret->treefile = ROSCXX_VAL (origin_to_treefile (*origin), error);
  CXX_TRY_VAR (kfv, rpmostreecxx::treefile_to_origin (**ret->treefile), error);
  ret->kf = std::move (kfv);

  ret->cached_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_enable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_install = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_local_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_local_fileoverride_packages
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_local_replace
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_remove = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  ret->cached_unconfigured_state
      = g_key_file_get_string (ret->kf, "origin", "unconfigured-state", NULL);

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

/* Mutability: getter */
rpmostreecxx::Refspec
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_base_refspec ();
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
rust::Vec<rust::String>
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages ();
}

bool
rpmostree_origin_has_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_packages ();
}

/* Mutability: getter */
bool
rpmostree_origin_has_modules_enable (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_modules_enable ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_local_packages ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_local_fileoverride_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_local_fileoverride_packages ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages_override_remove ();
}

bool
rpmostree_origin_has_overrides_remove_name (RpmOstreeOrigin *origin, const char *name)
{
  return (*origin->treefile)->has_packages_override_remove_name (rust::Str (name));
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages_override_replace_local ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_override_commit ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_etc_files ();
}

bool
rpmostree_origin_has_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_initramfs_etc_files ();
}

/* Mutability: getter */
bool
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_regenerate ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_args ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_unconfigured_state ();
}

/* Mutability: getter */
bool
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->may_require_local_assembly ();
}

/* Mutability: getter */
bool
rpmostree_origin_has_any_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_any_packages ();
}

/* Mutability: getter */
GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  // XXX: we should be able to make this conversion infallible
  return rpmostreecxx::treefile_to_origin (**origin->treefile);
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
bool
rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin, rust::Vec<rust::String> paths)
{
  auto changed = (*origin->treefile)->initramfs_etc_files_track (paths);
  if (changed)
    sync_origin (origin);
  return changed;
}

/* Mutability: setter */
bool
rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin,
                                              rust::Vec<rust::String> paths)
{
  auto changed = (*origin->treefile)->initramfs_etc_files_untrack (paths);
  if (changed)
    sync_origin (origin);
  return changed;
}

/* Mutability: setter */
bool
rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin)
{
  auto changed = (*origin->treefile)->initramfs_etc_files_untrack_all ();
  if (changed)
    sync_origin (origin);
  return changed;
}

/* Mutability: setter */
void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin, gboolean regenerate,
                                           rust::Vec<rust::String> args)
{
  (*origin->treefile)->set_initramfs_regenerate (regenerate, args);
  sync_origin (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin, const char *checksum)
{
  (*origin->treefile)->set_override_commit (checksum ?: "");
  sync_origin (origin);
}

/* Mutability: getter */
bool
rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_cliwrap ();
}

/* Mutability: setter */
void
rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, bool cliwrap)
{
  (*origin->treefile)->set_cliwrap (cliwrap);
  sync_origin (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin, const char *new_refspec,
                                    const char *custom_origin_url,
                                    const char *custom_origin_description)
{
  (*origin->treefile)
      ->rebase (new_refspec, custom_origin_url ?: "", custom_origin_description ?: "");
  sync_origin (origin);
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin, const char *new_refspec)
{
  rpmostree_origin_set_rebase_custom (origin, new_refspec, NULL, NULL);
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
rpmostree_origin_add_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                               gboolean allow_existing, gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->add_packages (packages, allow_existing), error);
  if (changed)
    sync_origin (origin);
  if (out_changed)
    *out_changed = changed;
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_local_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                                     gboolean allow_existing, gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->add_local_packages (packages, allow_existing), error);
  if (changed)
    sync_origin (origin);
  if (out_changed)
    *out_changed = changed;
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_local_fileoverride_packages (RpmOstreeOrigin *origin,
                                                  rust::Vec<rust::String> packages,
                                                  gboolean allow_existing, gboolean *out_changed,
                                                  GError **error)
{
  CXX_TRY_VAR (changed,
               (*origin->treefile)->add_local_fileoverride_packages (packages, allow_existing),
               error);
  if (changed)
    sync_origin (origin);
  if (out_changed)
    *out_changed = changed;
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
